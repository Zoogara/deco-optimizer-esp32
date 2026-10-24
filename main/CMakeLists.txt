idf_component_register(SRCS "main.c" "tmp_proto.c" "display.c" "wifi.c" "deco.c" "history.c" "ui.c" "mqtt.c" "settings.c" "provision.c" "web.c" "appstate.c" INCLUDE_DIRS "." EMBED_TXTFILES "web/index.html")
if(ORIENT_TEST)
  target_compile_definitions(${COMPONENT_LIB} PRIVATE ORIENT_TEST)
endif()
