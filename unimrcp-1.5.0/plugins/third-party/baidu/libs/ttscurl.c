//
// Created by fu on 3/5/18.
//

#include <memory.h>
#include "ttscurl.h"

const int MAX_HEADER_VALUE_LEN = 100;

static RETURN_CODE search_header(const char *buffer, size_t len, const char *key, char *value) {
    size_t len_key = strlen(key);
    char header_key[len_key + 1];
    header_key[len_key] = '\0';
    memcpy(header_key, buffer, len_key);
    if (strcasecmp(key, header_key) == 0 && (len - len_key) < MAX_HEADER_VALUE_LEN) {
        int len_value = len - len_key;
        value[len_value - 1] = '\0';
        memcpy(value, buffer + len_key + 1, len_value);
        return RETURN_OK;
    }
    return RETURN_FAIL;

}

size_t header_callback(char *buffer, size_t size, size_t nitems, struct http_result *result) {
    size_t len = size * nitems;
    char key[] = "Content-Type";
    char value[MAX_HEADER_VALUE_LEN];
    if (search_header(buffer, len, key, value) == RETURN_OK) {
        if (strstr(value, "audio/") != NULL) {
            result->has_error = 0;
        } else {
            fprintf(stderr, "Server return ERROR, %s : %s\n", key, value);

        }
    }
    return len;
}

size_t writefunc_data(void *ptr, size_t size, size_t nmemb, struct http_result *result) {
    printf("nmemb: %zu \n",nmemb);
    memcpy(result->content+result->size,ptr,nmemb);
    result->size = result->size+nmemb;
    printf("result->size: %d \n",result->size);
    return nmemb;
}
