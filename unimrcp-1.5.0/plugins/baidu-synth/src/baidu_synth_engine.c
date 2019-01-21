/*
 * Copyright 2008-2015 Arsen Chaloyan
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Mandatory rules concerning plugin implementation.
 * 1. Each plugin MUST implement a plugin/engine creator function
 *    with the exact signature and name (the main entry point)
 *        MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
 * 2. Each plugin MUST declare its version number
 *        MRCP_PLUGIN_VERSION_DECLARE
 * 3. One and only one response MUST be sent back to the received request.
 * 4. Methods (callbacks) of the MRCP engine channel MUST not block.
 *   (asynchronous response can be sent from the context of other thread)
 * 5. Methods (callbacks) of the MPF engine stream MUST not block.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include <curl/curl.h>
#include <memory.h>
#include "common.h"
#include "token.h"
#include "ttscurl.h"
#include "ttsmain.h"


#include "mrcp_synth_engine.h"
#include "apt_consumer_task.h"
#include "apt_log.h"
#include "mpf_buffer.h"

#define SYNTH_ENGINE_TASK_NAME "baidu Synth Engine"

const char TTS_SCOPE[] = "audio_tts_post";
const char API_TTS_URL[] = "http://tsn.baidu.com/text2audio"; // 可改为https

typedef struct baidu_synth_engine_t baidu_synth_engine_t;
typedef struct baidu_synth_channel_t baidu_synth_channel_t;
typedef struct baidu_synth_msg_t baidu_synth_msg_t;

/** Declaration of synthesizer engine methods */
static apt_bool_t baidu_synth_engine_destroy(mrcp_engine_t *engine);
static apt_bool_t baidu_synth_engine_open(mrcp_engine_t *engine);
static apt_bool_t baidu_synth_engine_close(mrcp_engine_t *engine);
static mrcp_engine_channel_t* baidu_synth_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool);

static const struct mrcp_engine_method_vtable_t engine_vtable = {
	baidu_synth_engine_destroy,
	baidu_synth_engine_open,
	baidu_synth_engine_close,
	baidu_synth_engine_channel_create
};


/** Declaration of synthesizer channel methods */
static apt_bool_t baidu_synth_channel_destroy(mrcp_engine_channel_t *channel);
static apt_bool_t baidu_synth_channel_open(mrcp_engine_channel_t *channel);
static apt_bool_t baidu_synth_channel_close(mrcp_engine_channel_t *channel);
static apt_bool_t baidu_synth_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request);

static const struct mrcp_engine_channel_method_vtable_t channel_vtable = {
	baidu_synth_channel_destroy,
	baidu_synth_channel_open,
	baidu_synth_channel_close,
	baidu_synth_channel_request_process
};

/** Declaration of synthesizer audio stream methods */
static apt_bool_t baidu_synth_stream_destroy(mpf_audio_stream_t *stream);
static apt_bool_t baidu_synth_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec);
static apt_bool_t baidu_synth_stream_close(mpf_audio_stream_t *stream);
static apt_bool_t baidu_synth_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame);

static const mpf_audio_stream_vtable_t audio_stream_vtable = {
	baidu_synth_stream_destroy,
	baidu_synth_stream_open,
	baidu_synth_stream_close,
	baidu_synth_stream_read,
	NULL,
	NULL,
	NULL,
	NULL
};

/** Declaration of baidu synthesizer engine */
struct baidu_synth_engine_t {
	apt_consumer_task_t    *task;
};

/** Declaration of baidu synthesizer channel */
struct baidu_synth_channel_t {
	/** Back pointer to engine */
	baidu_synth_engine_t   *baidu_engine;
	/** Engine channel base */
	mrcp_engine_channel_t *channel;

	/** Active (in-progress) speak request */
	mrcp_message_t        *speak_request;
	/** Pending stop response */
	mrcp_message_t        *stop_response;
	/** Estimated time to complete */
	apr_size_t             time_to_complete;
	/** Is paused */
	apt_bool_t             paused;
	/** Speech source (used instead of actual synthesis) */

	mpf_buffer_t          *audio_buffer;

};

typedef enum {
	baidu_SYNTH_MSG_OPEN_CHANNEL,
	baidu_SYNTH_MSG_CLOSE_CHANNEL,
	baidu_SYNTH_MSG_REQUEST_PROCESS
} baidu_synth_msg_type_e;

/** Declaration of baidu synthesizer task message */
struct baidu_synth_msg_t {
	baidu_synth_msg_type_e  type;
	mrcp_engine_channel_t *channel;
	mrcp_message_t        *request;
};


static apt_bool_t baidu_synth_msg_signal(baidu_synth_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request);
static apt_bool_t baidu_synth_msg_process(apt_task_t *task, apt_task_msg_t *msg);
static void baidu_synth_on_start(apt_task_t *task);
static void baidu_synth_on_terminate(apt_task_t *task);

/** Declare this macro to set plugin version */
MRCP_PLUGIN_VERSION_DECLARE

/**
 * Declare this macro to use log routine of the server, plugin is loaded from.
 * Enable/add the corresponding entry in logger.xml to set a cutsom log source priority.
 *    <source name="SYNTH-PLUGIN" priority="DEBUG" masking="NONE"/>
 */
MRCP_PLUGIN_LOG_SOURCE_IMPLEMENT(SYNTH_PLUGIN,"SYNTH-PLUGIN")

/** Use custom log source mark */
#define SYNTH_LOG_MARK   APT_LOG_MARK_DECLARE(SYNTH_PLUGIN)

/** Create baidu synthesizer engine */
MRCP_PLUGIN_DECLARE(mrcp_engine_t*) mrcp_plugin_create(apr_pool_t *pool)
{
	/* create baidu engine */
	baidu_synth_engine_t *baidu_engine = apr_palloc(pool,sizeof(baidu_synth_engine_t));
	apt_task_t *task;
	apt_task_vtable_t *vtable;
	apt_task_msg_pool_t *msg_pool;

	/* create task/thread to run baidu engine in the context of this task */
	msg_pool = apt_task_msg_pool_create_dynamic(sizeof(baidu_synth_msg_t),pool);
	baidu_engine->task = apt_consumer_task_create(baidu_engine,msg_pool,pool);
	if(!baidu_engine->task) {
		return NULL;
	}
	task = apt_consumer_task_base_get(baidu_engine->task);
	apt_task_name_set(task,SYNTH_ENGINE_TASK_NAME);
	vtable = apt_task_vtable_get(task);
	if(vtable) {
		vtable->process_msg = baidu_synth_msg_process;
		// vtable->on_pre_run = baidu_synth_on_start;
		// vtable->on_post_run = baidu_synth_on_terminate;
	}

	/* create engine base */
	return mrcp_engine_create(
				MRCP_SYNTHESIZER_RESOURCE, /* MRCP resource identifier */
				baidu_engine,               /* object to associate */
				&engine_vtable,            /* virtual methods table of engine */
				pool);                     /* pool to allocate memory from */
}

/** Destroy synthesizer engine */
static apt_bool_t baidu_synth_engine_destroy(mrcp_engine_t *engine)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] destroy synthesizer engine");
	baidu_synth_engine_t *baidu_engine = engine->obj;
	if(baidu_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(baidu_engine->task);
		apt_task_destroy(task);
		baidu_engine->task = NULL;
	}
	return TRUE;
}

/** Open synthesizer engine */
static apt_bool_t baidu_synth_engine_open(mrcp_engine_t *engine)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] open synthesizer engine");
    curl_global_init(CURL_GLOBAL_ALL);
	baidu_synth_engine_t *baidu_engine = engine->obj;
	if(baidu_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(baidu_engine->task);
		apt_task_start(task);
	}
	return mrcp_engine_open_respond(engine,TRUE);
}

/** Close synthesizer engine */
static apt_bool_t baidu_synth_engine_close(mrcp_engine_t *engine)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] close synthesizer engine");
    curl_global_cleanup();
	baidu_synth_engine_t *baidu_engine = engine->obj;
	if(baidu_engine->task) {
		apt_task_t *task = apt_consumer_task_base_get(baidu_engine->task);
		apt_task_terminate(task,TRUE);
	}
	return mrcp_engine_close_respond(engine);
}

/** Create baidu synthesizer channel derived from engine channel base */
static mrcp_engine_channel_t* baidu_synth_engine_channel_create(mrcp_engine_t *engine, apr_pool_t *pool)
{
	apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] create synthesizer channel");
	mpf_stream_capabilities_t *capabilities;
	mpf_termination_t *termination;

	/* create baidu synth channel */
	baidu_synth_channel_t *synth_channel = apr_palloc(pool,sizeof(baidu_synth_channel_t));
	synth_channel->baidu_engine = engine->obj;
	synth_channel->speak_request = NULL;
	synth_channel->stop_response = NULL;
	synth_channel->time_to_complete = 0;
	synth_channel->paused = FALSE;
	synth_channel->audio_buffer = NULL;

	capabilities = mpf_source_stream_capabilities_create(pool);
	mpf_codec_capabilities_add(
			&capabilities->codecs,
			MPF_SAMPLE_RATE_8000 | MPF_SAMPLE_RATE_16000,
			"LPCM");

	/* create media termination */
	termination = mrcp_engine_audio_termination_create(
			synth_channel,        /* object to associate */
			&audio_stream_vtable, /* virtual methods table of audio stream */
			capabilities,         /* stream capabilities */
			pool);                /* pool to allocate memory from */

	/* create engine channel base */
	synth_channel->channel = mrcp_engine_channel_create(
			engine,               /* engine */
			&channel_vtable,      /* virtual methods table of engine channel */
			synth_channel,        /* object to associate */
			termination,          /* associated media termination */
			pool);                /* pool to allocate memory from */

	synth_channel->audio_buffer = mpf_buffer_create(pool);
	return synth_channel->channel;
}

/** Destroy engine channel */
static apt_bool_t baidu_synth_channel_destroy(mrcp_engine_channel_t *channel)
{
	/* nothing to destroy */
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] Destroy engine synthesizer channel");
	return TRUE;
}

/** Open engine channel (asynchronous response MUST be sent)*/
static apt_bool_t baidu_synth_channel_open(mrcp_engine_channel_t *channel)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] Open engine synthesizer channel");
	return baidu_synth_msg_signal(baidu_SYNTH_MSG_OPEN_CHANNEL,channel,NULL);
}

/** Close engine channel (asynchronous response MUST be sent)*/
static apt_bool_t baidu_synth_channel_close(mrcp_engine_channel_t *channel)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] Close engine synthesizer channel");
	return baidu_synth_msg_signal(baidu_SYNTH_MSG_CLOSE_CHANNEL,channel,NULL);
}

/** Process MRCP channel request (asynchronous response MUST be sent)*/
static apt_bool_t baidu_synth_channel_request_process(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_channel_request_process");
	return baidu_synth_msg_signal(baidu_SYNTH_MSG_REQUEST_PROCESS,channel,request);
}

static apt_bool_t baidu_synth_text_to_speech(struct tts_config *config, const char* src_text, const char* params, mpf_buffer_t *buffer) {
	    char token[MAX_TOKEN_SIZE];     // 获取token
	// 填写网页上申请的appkey 如 g_api_key="g8eBUMSokVB1BHGmgxxxxxx"
	    char api_key[] = "4E1BG9lTnlSeIf1NQFlrSq6h";
	    // 填写网页上申请的APP SECRET 如 $secretKey="94dc99566550d87f8fa8ece112xxxxx"
	    char secret_key[] = "544ca4657ba8002e3dea3ac2f5fdd241";
	    // 发音人选择, 0为普通女声，1为普通男生，3为情感合成-度逍遥，4为情感合成-度丫丫，默认为普通女声
	    int per = 0;
	    // 语速，取值0-9，默认为5中语速
	    int spd = 5;
	    // #音调，取值0-9，默认为5中语调
	    int pit = 5;
	    // #音量，取值0-9，默认为5中音量
	    int vol = 5;
	    // 下载的文件格式, 3：mp3(default) 4： pcm-16k 5： pcm-8k 6. wav
		int aue = 5;

	    // 将上述参数填入config中
	    snprintf(config->api_key, sizeof(config->api_key), "%s", api_key);
	    snprintf(config->secret_key, sizeof(config->secret_key), "%s", secret_key);
	    snprintf(config->text, strlen(src_text), "%s", src_text);
	    config->text_len = strlen(src_text) - 1;
	    snprintf(config->cuid, sizeof(config->cuid), "1234567C");
	    config->per = per;
	    config->spd = spd;
	    config->pit = pit;
	    config->vol = vol;
		config->aue = aue;

		// aue对应的格式，format
		const char formats[4][4] = {"mp3", "pcm", "pcm", "wav"};
		snprintf(config->format, sizeof(config->format), formats[aue - 3]);
	    RETURN_CODE res = speech_get_token(config->api_key, config->secret_key, TTS_SCOPE, token);
        if (res == RETURN_OK) {
            // 调用识别接口
            char params[200 + config->text_len * 9];
            CURL *curl = curl_easy_init(); // 需要释放
            char *cuid = curl_easy_escape(curl, config->cuid, strlen(config->cuid)); // 需要释放
            char *textemp = curl_easy_escape(curl, config->text, config->text_len); // 需要释放
        	char *tex = curl_easy_escape(curl, textemp, strlen(textemp)); // 需要释放
        	curl_free(textemp);
        	char params_pattern[] = "ctp=1&lan=zh&cuid=%s&tok=%s&tex=%s&per=%d&spd=%d&pit=%d&vol=%d&aue=%d";
            snprintf(params, sizeof(params), params_pattern , cuid, token, tex,
                     config->per, config->spd, config->pit, config->vol, config->aue);

        	char url[sizeof(params) + 200];
        	snprintf(url, sizeof(url), "%s?%s", API_TTS_URL, params);
            printf("test in browser: %s\n", url);
            curl_free(cuid);
          	curl_free(tex);
        	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, params);
            curl_easy_setopt(curl, CURLOPT_URL, API_TTS_URL);
            curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5); // 连接5s超时
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60); // 整体请求60s超时
            void *content =malloc(1024*1024);
            struct http_result result = {1, config->format ,NULL,content,0};
            curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback); // 检查头部
            curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writefunc_data);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result);  // 需要释放
            curl_easy_setopt(curl, CURLOPT_VERBOSE, ENABLE_CURL_VERBOSE);
            CURLcode res_curl = curl_easy_perform(curl);
			mpf_buffer_audio_write(buffer, content, result.size);
			free(content);
            printf("free content\n");
            RETURN_CODE res = RETURN_OK;
            if (res_curl != CURLE_OK) {
                // curl 失败
                snprintf(g_demo_error_msg, BUFFER_ERROR_SIZE, "perform curl error:%d, %s.\n", res,
                         curl_easy_strerror(res_curl));
                res = ERROR_TTS_CURL;
            }

            curl_easy_cleanup(curl);
        }

	return TRUE;
}

/** Process SPEAK request */
static apt_bool_t synth_response_construct(mrcp_message_t *response, mrcp_status_code_e status_code, mrcp_synth_completion_cause_e completion_cause)
{
	mrcp_synth_header_t *synth_header = mrcp_resource_header_prepare(response);
	if(!synth_header) {
		return FALSE;
	}

	response->start_line.status_code = status_code;
	synth_header->completion_cause = completion_cause;
	mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_COMPLETION_CAUSE);
	return TRUE;
}

/** Process SPEAK request */
static apt_bool_t baidu_synth_channel_speak(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
	apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] process speak request");
	apt_str_t *body;
	const char* session_begin_params = "voice_name = xiaoyan, text_encoding = utf8, sample_rate = 8000, speed = 50, volume = 50, pitch = 50, rdn = 2";
	baidu_synth_channel_t *synth_channel = channel->method_obj;
	const mpf_codec_descriptor_t *descriptor = mrcp_engine_source_stream_codec_get(channel);

	if(!descriptor) {
		apt_log(SYNTH_LOG_MARK,APT_PRIO_WARNING,"Failed to Get Codec Descriptor " APT_SIDRES_FMT, MRCP_MESSAGE_SIDRES(request));
		response->start_line.status_code = MRCP_STATUS_CODE_METHOD_FAILED;
		return FALSE;
	}

	synth_channel->speak_request = request;
	body = &synth_channel->speak_request->body;
	if(!body->length) {
		synth_channel->speak_request = NULL;
		synth_response_construct(response,MRCP_STATUS_CODE_MISSING_PARAM,SYNTHESIZER_COMPLETION_CAUSE_ERROR);
		mrcp_engine_channel_message_send(synth_channel->channel,response);
		return FALSE;
	}

	synth_channel->time_to_complete = 0;

	response->start_line.request_state = MRCP_REQUEST_STATE_INPROGRESS;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	struct tts_config config;
	baidu_synth_text_to_speech(&config, body->buf, session_begin_params, synth_channel->audio_buffer);
	mpf_buffer_event_write(synth_channel->audio_buffer, MEDIA_FRAME_TYPE_EVENT);
	return TRUE;
}

static APR_INLINE baidu_synth_channel_t* baidu_synth_channel_get(apt_task_t *task)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_channel_get");
	apt_consumer_task_t *consumer_task = apt_task_object_get(task);
	return apt_consumer_task_object_get(consumer_task);
}

static void baidu_synth_on_start(apt_task_t *task)
{
	baidu_synth_channel_t *synth_channel = baidu_synth_channel_get(task);
	apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] Speak task started");
	mrcp_engine_channel_open_respond(synth_channel->channel,TRUE);
}

static void baidu_synth_on_terminate(apt_task_t *task)
{
	baidu_synth_channel_t *synth_channel = baidu_synth_channel_get(task);
	apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] Speak task terminated");
	mrcp_engine_channel_close_respond(synth_channel->channel);
}

/** Process STOP request */
static apt_bool_t baidu_synth_channel_stop(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] synth_channel STOP");
	baidu_synth_channel_t *synth_channel = channel->method_obj;
	/* store the request, make sure there is no more activity and only then send the response */
	synth_channel->stop_response = response;
	return TRUE;
}

/** Process PAUSE request */
static apt_bool_t baidu_synth_channel_pause(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] synth_channel PAUSE");
	baidu_synth_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = TRUE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process RESUME request */
static apt_bool_t baidu_synth_channel_resume(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] synth_channel RESUME");
	baidu_synth_channel_t *synth_channel = channel->method_obj;
	synth_channel->paused = FALSE;
	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process SET-PARAMS request */
static apt_bool_t baidu_synth_channel_set_params(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_channel_set_params ");
	mrcp_synth_header_t *req_synth_header;
	/* get synthesizer header */
	req_synth_header = mrcp_resource_header_get(request);
	if(req_synth_header) {
		/* check voice age header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_AGE) == TRUE) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set Voice Age [%"APR_SIZE_T_FMT"]",
				req_synth_header->voice_param.age);
		}
		/* check voice name header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			apt_log(SYNTH_LOG_MARK,APT_PRIO_INFO,"Set Voice Name [%s]",
				req_synth_header->voice_param.name.buf);
		}
	}

	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Process GET-PARAMS request */
static apt_bool_t baidu_synth_channel_get_params(mrcp_engine_channel_t *channel, mrcp_message_t *request, mrcp_message_t *response)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_channel_get_params ");
	mrcp_synth_header_t *req_synth_header;
	/* get synthesizer header */
	req_synth_header = mrcp_resource_header_get(request);
	if(req_synth_header) {
		mrcp_synth_header_t *res_synth_header = mrcp_resource_header_prepare(response);
		/* check voice age header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_AGE) == TRUE) {
			res_synth_header->voice_param.age = 25;
			mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_VOICE_AGE);
		}
		/* check voice name header */
		if(mrcp_resource_header_property_check(request,SYNTHESIZER_HEADER_VOICE_NAME) == TRUE) {
			apt_string_set(&res_synth_header->voice_param.name,"David");
			mrcp_resource_header_property_add(response,SYNTHESIZER_HEADER_VOICE_NAME);
		}
	}

	/* send asynchronous response */
	mrcp_engine_channel_message_send(channel,response);
	return TRUE;
}

/** Dispatch MRCP request */
static apt_bool_t baidu_synth_channel_request_dispatch(mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_channel_request_dispatch s%",request->start_line.method_id);
	apt_bool_t processed = FALSE;
	mrcp_message_t *response = mrcp_response_create(request,request->pool);
	switch(request->start_line.method_id) {
		case SYNTHESIZER_SET_PARAMS:
			processed = baidu_synth_channel_set_params(channel,request,response);
			break;
		case SYNTHESIZER_GET_PARAMS:
			processed = baidu_synth_channel_get_params(channel,request,response);
			break;
		case SYNTHESIZER_SPEAK:
			processed = baidu_synth_channel_speak(channel,request,response);
			break;
		case SYNTHESIZER_STOP:
			processed = baidu_synth_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_PAUSE:
			processed = baidu_synth_channel_pause(channel,request,response);
			break;
		case SYNTHESIZER_RESUME:
			processed = baidu_synth_channel_resume(channel,request,response);
			break;
		case SYNTHESIZER_BARGE_IN_OCCURRED:
			processed = baidu_synth_channel_stop(channel,request,response);
			break;
		case SYNTHESIZER_CONTROL:
			break;
		case SYNTHESIZER_DEFINE_LEXICON:
			break;
		default:
			break;
	}
	if(processed == FALSE) {
		/* send asynchronous response for not handled request */
		mrcp_engine_channel_message_send(channel,response);
	}
	return TRUE;
}

/** Callback is called from MPF engine context to destroy any additional data associated with audio stream */
static apt_bool_t baidu_synth_stream_destroy(mpf_audio_stream_t *stream)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_stream_destroy");
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action before open */
static apt_bool_t baidu_synth_stream_open(mpf_audio_stream_t *stream, mpf_codec_t *codec)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_stream_open");
	return TRUE;
}

/** Callback is called from MPF engine context to perform any action after close */
static apt_bool_t baidu_synth_stream_close(mpf_audio_stream_t *stream)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_stream_close");
	return TRUE;
}

/** Raise SPEAK-COMPLETE event */
static apt_bool_t baidu_synth_speak_complete_raise(baidu_synth_channel_t *synth_channel)
{
	mrcp_message_t *message = 0;
	mrcp_synth_header_t * synth_header = 0;
	apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_speak_complete_raise");

	if (!synth_channel->speak_request) {
		return FALSE;
	}

	message = mrcp_event_create(
						synth_channel->speak_request,
						SYNTHESIZER_SPEAK_COMPLETE,
						synth_channel->speak_request->pool);
	if (!message) {
		return FALSE;
	}

	/* get/allocate synthesizer header */
	synth_header = (mrcp_synth_header_t *) mrcp_resource_header_prepare(message);
	if (synth_header) {
		/* set completion cause */
		synth_header->completion_cause = SYNTHESIZER_COMPLETION_CAUSE_NORMAL;
		mrcp_resource_header_property_add(message,SYNTHESIZER_HEADER_COMPLETION_CAUSE);
	}
	/* set request state */
	message->start_line.request_state = MRCP_REQUEST_STATE_COMPLETE;

	synth_channel->speak_request = NULL;
	/* send asynch event */
	return mrcp_engine_channel_message_send(synth_channel->channel,message);
}

/** Callback is called from MPF engine context to read/get new frame */
static apt_bool_t baidu_synth_stream_read(mpf_audio_stream_t *stream, mpf_frame_t *frame)
{
	// apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] stream read");
	baidu_synth_channel_t *synth_channel = stream->obj;
	/* check if STOP was requested */
	if(synth_channel->stop_response) {
         apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] synth_channel stop_response");
		/* send asynchronous response to STOP request */
		mrcp_engine_channel_message_send(synth_channel->channel,synth_channel->stop_response);
		synth_channel->stop_response = NULL;
		synth_channel->speak_request = NULL;
		synth_channel->paused = FALSE;
		return TRUE;
	}

	/* check if there is active SPEAK request and it isn't in paused state */
	if(synth_channel->speak_request && synth_channel->paused == FALSE) {
		// apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] read audio buffer to frame");
		/* normal processing */
		mpf_buffer_frame_read(synth_channel->audio_buffer,frame);
			/* raise SPEAK-COMPLETE event */
		if((frame->type & MEDIA_FRAME_TYPE_EVENT) == MEDIA_FRAME_TYPE_EVENT) {
			frame->type &= ~MEDIA_FRAME_TYPE_EVENT;
			baidu_synth_speak_complete_raise(synth_channel);
		}
	}
	return TRUE;
}

static apt_bool_t baidu_synth_msg_signal(baidu_synth_msg_type_e type, mrcp_engine_channel_t *channel, mrcp_message_t *request)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_msg_signal");
	apt_bool_t status = FALSE;
	baidu_synth_channel_t *baidu_channel = channel->method_obj;
	baidu_synth_engine_t *baidu_engine = baidu_channel->baidu_engine;
	apt_task_t *task = apt_consumer_task_base_get(baidu_engine->task);
	apt_task_msg_t *msg = apt_task_msg_get(task);
	if(msg) {
		baidu_synth_msg_t *baidu_msg;
		msg->type = TASK_MSG_USER;
		baidu_msg = (baidu_synth_msg_t*) msg->data;

		baidu_msg->type = type;
		baidu_msg->channel = channel;
		baidu_msg->request = request;
		status = apt_task_msg_signal(task,msg);
	}
	return status;
}

static apt_bool_t baidu_synth_msg_process(apt_task_t *task, apt_task_msg_t *msg)
{
    apt_log(APT_LOG_MARK, APT_PRIO_INFO, "[baidu tts] baidu_synth_msg_process");
	baidu_synth_msg_t *baidu_msg = (baidu_synth_msg_t*)msg->data;
	switch(baidu_msg->type) {
		case baidu_SYNTH_MSG_OPEN_CHANNEL:
			/* open channel and send asynch response */
			mrcp_engine_channel_open_respond(baidu_msg->channel,TRUE);
			break;
		case baidu_SYNTH_MSG_CLOSE_CHANNEL:
			/* close channel, make sure there is no activity and send asynch response */
			mrcp_engine_channel_close_respond(baidu_msg->channel);
			break;
		case baidu_SYNTH_MSG_REQUEST_PROCESS:
			baidu_synth_channel_request_dispatch(baidu_msg->channel,baidu_msg->request);
			break;
		default:
			break;
	}
	return TRUE;
}
