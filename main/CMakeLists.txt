set(srcs main.c tmp_proto.c wifi.c deco.c history.c mqtt.c settings.c provision.c web.c appstate.c evlog.c)
if(CONFIG_DECO_HEADLESS)
    list(APPEND srcs headless.c)          # no-op display/UI
else()
    list(APPEND srcs display.c ui.c screensaver.c)
endif()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "." EMBED_TXTFILES "web/index.html")
