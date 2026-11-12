set(srcs main.c tmp_proto.c wifi.c deco.c history.c mqtt.c settings.c provision.c web.c appstate.c evlog.c deco_nodes.c pings.c)
if(CONFIG_DECO_HEADLESS)
    list(APPEND srcs headless.c)          # no-op display/UI
else()
    list(APPEND srcs display.c ui.c screensaver.c)
endif()
# libssh2 keeps two 35 KB packet buffers inside its session struct (about 79 KB in one block), which is more
# contiguous heap than this chip reliably has. Our SSH traffic is tiny, so shrink the buffers and the packet size
# we advertise (together they must stay consistent). Done at configure time because the component is downloaded.
set(_ssh ${CMAKE_SOURCE_DIR}/managed_components/skuodi__libssh2_esp/libssh2)
foreach(_edit "src/libssh2_priv.h|#define MAX_SSH_PACKET_LEN 35000|#define MAX_SSH_PACKET_LEN 12000"
              "include/libssh2.h|#define LIBSSH2_PACKET_MAXPAYLOAD   40000|#define LIBSSH2_PACKET_MAXPAYLOAD   12000"
              "include/libssh2.h|#define LIBSSH2_CHANNEL_PACKET_DEFAULT  32768|#define LIBSSH2_CHANNEL_PACKET_DEFAULT  8192")
    string(REPLACE "|" ";" _e "${_edit}")
    list(GET _e 0 _f)
    list(GET _e 1 _from)
    list(GET _e 2 _to)
    if(EXISTS ${_ssh}/${_f})
        file(READ ${_ssh}/${_f} _txt)
        string(FIND "${_txt}" "${_from}" _pos)
        if(NOT _pos EQUAL -1)
            string(REPLACE "${_from}" "${_to}" _txt "${_txt}")
            file(WRITE ${_ssh}/${_f} "${_txt}")
            message(STATUS "libssh2 patched: ${_to}")
        endif()
    endif()
endforeach()
idf_component_register(SRCS ${srcs} INCLUDE_DIRS "." EMBED_TXTFILES "web/index.html" EMBED_FILES "web/icon.png")
