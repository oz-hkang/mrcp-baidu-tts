AM_CPPFLAGS                = $(UNIMRCP_PLUGIN_INCLUDES)

plugin_LTLIBRARIES         = baidusynth.la

baidusynth_la_SOURCES       = src/baidu_synth_engine.c

baidusynth_la_LDFLAGS       = $(UNIMRCP_PLUGIN_OPTS) \
                             -L$(top_srcdir)/plugins/third-party/baidu/libs \
                             -lbtts -lcurl
baidusynth_ladir            = $(libdir)

include $(top_srcdir)/build/rules/uniplugin.am

UNIMRCP_PLUGIN_INCLUDES     += -I$(top_srcdir)/plugins/third-party/baidu/include
