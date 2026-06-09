#ifndef MODGUI_HOST_COMMON_H
#define MODGUI_HOST_COMMON_H

/* Base URI for all identifiers */
#define MODGUI_HOST_URI           "urn:modgui-host"
#define MODGUI_HOST_PLUGIN_URI    MODGUI_HOST_URI ":plugin"
#define MODGUI_HOST_UI_URI        MODGUI_HOST_URI ":ui"

/* State / patch property URIs */
#define MODGUI_HOSTED_PLUGIN_URI  MODGUI_HOST_URI ":hostedPluginURI"
#define MODGUI_PARAM_SYMBOL       MODGUI_HOST_URI ":paramSymbol"
#define MODGUI_PARAM_VALUE        MODGUI_HOST_URI ":paramValue"
#define MODGUI_PARAM_CHANGE       MODGUI_HOST_URI ":paramChange"

/* modgui namespace */
#define MODGUI_NS                 "http://moddevices.com/ns/modgui#"
#define MODGUI_GUI                MODGUI_NS "gui"
#define MODGUI_TEMPLATE_FILE      MODGUI_NS "templateFile"
#define MODGUI_RESOURCES_DIR      MODGUI_NS "resourcesDirectory"
#define MODGUI_JAVASCRIPT         MODGUI_NS "javascript"
#define MODGUI_STYLESHEET         MODGUI_NS "stylesheet"

/* Port indices (wrapper plugin) */
enum ModguiPortIndex {
    PORT_AUDIO_IN_L  = 0,
    PORT_AUDIO_IN_R  = 1,
    PORT_AUDIO_OUT_L = 2,
    PORT_AUDIO_OUT_R = 3,
    PORT_EVENTS_IN   = 4,
    PORT_EVENTS_OUT  = 5,
    PORT_COUNT       = 6,
};

#define ATOM_BUF_SIZE   (4096u)
#define MAX_BLOCK_SIZE  (8192u)

#endif /* MODGUI_HOST_COMMON_H */
