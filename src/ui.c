#include <lv2/core/lv2.h>
#include <lv2/core/lv2_util.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>
#include <lv2/patch/patch.h>
#include <lv2/ui/ui.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>

#include <lilv/lilv.h>

#include <gtk/gtk.h>

/* Both webkit2gtk-4.0 and webkit2gtk-4.1 use the same header path.
 * The WEBKIT_API_40 flag only controls which JS evaluation API to use. */
#include <webkit2/webkit2.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "common.h"

/* ── Types ───────────────────────────────────────────────────────────────── */

typedef struct {
    char*  name;           /* malloc-owned */
    char*  uri;            /* malloc-owned */
    gchar* bundle_path;    /* g_filename_from_uri, g_free */
    gchar* template_file;  /* g_filename_from_uri, g_free */
    gchar* resources_dir;  /* g_filename_from_uri, g_free */
} PluginInfo;

typedef struct {
    /* LV2 callbacks */
    LV2UI_Write_Function write_function;
    LV2UI_Controller     controller;

    /* LV2 features */
    LV2_URID_Map*   map;
    LV2_Log_Logger  logger;

    struct {
        LV2_URID atom_Object;
        LV2_URID atom_String;
        LV2_URID atom_Float;
        LV2_URID atom_Sequence;
        LV2_URID atom_event_transfer;
        LV2_URID patch_Set;
        LV2_URID patch_property;
        LV2_URID patch_value;
        LV2_URID hosted_plugin_uri;
        LV2_URID param_change;
        LV2_URID param_symbol;
        LV2_URID param_value;
    } urid;

    LV2_Atom_Forge forge;

    /* GTK widgets */
    GtkWidget*          root;       /* top-level VBox returned to host */
    GtkWidget*          header_bar;
    GtkWidget*          plugin_label;
    GtkWidget*          load_btn;
    GtkWidget*          webview;

    /* Atom forge output buffer */
    uint8_t             forge_buf[4096];

    /* Current hosted plugin info (NULL if none) */
    PluginInfo*         current_plugin;

    /* All discovered plugins with modgui (for picker) */
    PluginInfo*         plugin_list;
    int                 n_plugins;

    /* Lilv world for discovery */
    LilvWorld*          world;

    /* Bridge JS path */
    char*               bridge_js_path;
} ModguiHostUI;

/* ── Forward declarations ────────────────────────────────────────────────── */
static void     discover_plugins(ModguiHostUI* ui);
static void     free_plugin_info(PluginInfo* p);
static void     on_load_plugin_clicked(GtkButton* btn, gpointer user_data);
static void     load_modgui(ModguiHostUI* ui, const PluginInfo* p);
static void     send_hosted_uri(ModguiHostUI* ui, const char* uri);
static void     send_param_change(ModguiHostUI* ui,
                                   const char* symbol, float value);
static void     on_script_message(WebKitUserContentManager* mgr,
#ifdef WEBKIT_API_40
                                   WebKitJavascriptResult* result,
#else
                                   JSCValue*               result,
#endif
                                   gpointer user_data);
static gboolean run_js(ModguiHostUI* ui, const char* script);

/* ── Plugin discovery ────────────────────────────────────────────────────── */

static void discover_plugins(ModguiHostUI* ui)
{
    ui->n_plugins   = 0;
    ui->plugin_list = NULL;

    LilvNode* modgui_gui = lilv_new_uri(ui->world, MODGUI_GUI);
    const LilvPlugins* all = lilv_world_get_all_plugins(ui->world);

    /* First pass: count */
    int count = 0;
    LILV_FOREACH(plugins, it, all) {
        const LilvPlugin* lp = lilv_plugins_get(all, it);
        LilvNodes* vals = lilv_plugin_get_value(lp, modgui_gui);
        if (vals) { count++; lilv_nodes_free(vals); }
    }

    if (count == 0) {
        lilv_node_free(modgui_gui);
        return;
    }

    ui->plugin_list = (PluginInfo*)calloc((size_t)count, sizeof(PluginInfo));
    int idx = 0;

    LilvNode* tmpl_node    = lilv_new_uri(ui->world, MODGUI_TEMPLATE_FILE);
    LilvNode* res_dir_node = lilv_new_uri(ui->world, MODGUI_RESOURCES_DIR);

    LILV_FOREACH(plugins, it, all) {
        const LilvPlugin* lp = lilv_plugins_get(all, it);
        LilvNodes* gui_nodes = lilv_plugin_get_value(lp, modgui_gui);
        if (!gui_nodes) continue;

        PluginInfo* p = &ui->plugin_list[idx++];

        /* Name */
        LilvNode* name_node = lilv_plugin_get_name(lp);
        p->name = name_node ? strdup(lilv_node_as_string(name_node)) : NULL;
        lilv_node_free(name_node);

        /* URI */
        p->uri = strdup(lilv_node_as_uri(lilv_plugin_get_uri(lp)));

        /* Bundle path */
        const LilvNode* bundle = lilv_plugin_get_bundle_uri(lp);
        if (bundle) {
            p->bundle_path =
                g_filename_from_uri(lilv_node_as_uri(bundle), NULL, NULL);
        }

        /* modgui:gui node → template file */
        LilvNode* gui_node = lilv_nodes_get_first(gui_nodes);
        if (gui_node) {
            LilvNode* tmpl = lilv_world_get(ui->world, gui_node, tmpl_node, NULL);
            if (tmpl) {
                p->template_file =
                    g_filename_from_uri(lilv_node_as_uri(tmpl), NULL, NULL);
                lilv_node_free(tmpl);
            }
            LilvNode* rdir = lilv_world_get(ui->world, gui_node, res_dir_node, NULL);
            if (rdir) {
                p->resources_dir =
                    g_filename_from_uri(lilv_node_as_uri(rdir), NULL, NULL);
                lilv_node_free(rdir);
            }
        }

        lilv_nodes_free(gui_nodes);
    }

    lilv_node_free(modgui_gui);
    lilv_node_free(tmpl_node);
    lilv_node_free(res_dir_node);

    ui->n_plugins = idx;
}

static void free_plugin_info(PluginInfo* p)
{
    if (!p) return;
    free(p->name);
    free(p->uri);
    /* bundle_path, template_file, resources_dir are from g_filename_from_uri */
    g_free(p->bundle_path);
    g_free(p->template_file);
    g_free(p->resources_dir);
    memset(p, 0, sizeof(*p));
}

/* ── JS helpers ──────────────────────────────────────────────────────────── */

static gboolean run_js(ModguiHostUI* ui, const char* script)
{
    if (!ui->webview) return FALSE;
#ifdef WEBKIT_API_40
    webkit_web_view_run_javascript((WebKitWebView*)ui->webview,
                                   script, NULL, NULL, NULL);
#else
    webkit_web_view_evaluate_javascript((WebKitWebView*)ui->webview,
                                        script, -1, NULL, NULL, NULL,
                                        NULL, NULL);
#endif
    return TRUE;
}

/* ── Script message handler (JS → C) ────────────────────────────────────── */

static void
on_script_message(WebKitUserContentManager* mgr,
#ifdef WEBKIT_API_40
                  WebKitJavascriptResult*   result,
#else
                  JSCValue*                 result,
#endif
                  gpointer                  user_data)
{
    (void)mgr;
    ModguiHostUI* ui = (ModguiHostUI*)user_data;

#ifdef WEBKIT_API_40
    JSCValue* val = webkit_javascript_result_get_js_value(result);
#else
    JSCValue* val = result;
#endif

    if (!jsc_value_is_object(val)) return;

    JSCValue* type_v   = jsc_value_object_get_property(val, "type");
    JSCValue* symbol_v = jsc_value_object_get_property(val, "symbol");
    JSCValue* value_v  = jsc_value_object_get_property(val, "value");

    if (!type_v || !jsc_value_is_string(type_v)) goto cleanup;

    char* type_str = jsc_value_to_string(type_v);
    if (type_str && strcmp(type_str, "parameterChange") == 0) {
        if (symbol_v && jsc_value_is_string(symbol_v) &&
            value_v  && jsc_value_is_number(value_v)) {
            char*  sym = jsc_value_to_string(symbol_v);
            double dval = jsc_value_to_double(value_v);
            send_param_change(ui, sym, (float)dval);
            g_free(sym);
        }
    }
    g_free(type_str);

cleanup:
    if (type_v)   g_object_unref(type_v);
    if (symbol_v) g_object_unref(symbol_v);
    if (value_v)  g_object_unref(value_v);
}

/* ── Atom helpers ────────────────────────────────────────────────────────── */

static void send_hosted_uri(ModguiHostUI* ui, const char* uri)
{
    lv2_atom_forge_set_buffer(&ui->forge, ui->forge_buf, sizeof(ui->forge_buf));

    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_object(&ui->forge, &frame, 0, ui->urid.patch_Set);
    lv2_atom_forge_key(&ui->forge, ui->urid.patch_property);
    lv2_atom_forge_urid(&ui->forge, ui->urid.hosted_plugin_uri);
    lv2_atom_forge_key(&ui->forge, ui->urid.patch_value);
    lv2_atom_forge_string(&ui->forge, uri, strlen(uri));
    lv2_atom_forge_pop(&ui->forge, &frame);

    LV2_Atom* atom = (LV2_Atom*)ui->forge_buf;
    ui->write_function(ui->controller, PORT_EVENTS_IN,
                       lv2_atom_total_size(atom),
                       ui->urid.atom_event_transfer,
                       atom);
}

static void send_param_change(ModguiHostUI* ui,
                               const char* symbol, float value)
{
    lv2_atom_forge_set_buffer(&ui->forge, ui->forge_buf, sizeof(ui->forge_buf));

    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_object(&ui->forge, &frame, 0, ui->urid.param_change);
    lv2_atom_forge_key(&ui->forge, ui->urid.param_symbol);
    lv2_atom_forge_string(&ui->forge, symbol, strlen(symbol));
    lv2_atom_forge_key(&ui->forge, ui->urid.param_value);
    lv2_atom_forge_float(&ui->forge, value);
    lv2_atom_forge_pop(&ui->forge, &frame);

    LV2_Atom* atom = (LV2_Atom*)ui->forge_buf;
    ui->write_function(ui->controller, PORT_EVENTS_IN,
                       lv2_atom_total_size(atom),
                       ui->urid.atom_event_transfer,
                       atom);
}

/* ── Load modgui into WebKit ─────────────────────────────────────────────── */

static void load_modgui(ModguiHostUI* ui, const PluginInfo* p)
{
    if (!p->template_file) {
        lv2_log_warning(&ui->logger,
                        "modgui-host: plugin %s has no template file\n",
                        p->name ? p->name : p->uri);
        return;
    }

    /* Read template HTML */
    gchar* html = NULL;
    gsize  html_len = 0;
    GError* err = NULL;
    if (!g_file_get_contents(p->template_file, &html, &html_len, &err)) {
        lv2_log_error(&ui->logger,
                      "modgui-host: cannot read %s: %s\n",
                      p->template_file, err ? err->message : "?");
        g_clear_error(&err);
        return;
    }

    /* Build base URI from the resources directory (or bundle) */
    const char* base_dir = p->resources_dir ? p->resources_dir : p->bundle_path;
    char* base_uri = NULL;
    if (base_dir) {
        base_uri = g_filename_to_uri(base_dir, NULL, NULL);
    }

    /* Prepare bridge script injection */
    gchar* bridge_js = NULL;
    if (ui->bridge_js_path) {
        gsize blen = 0;
        g_file_get_contents(ui->bridge_js_path, &bridge_js, &blen, NULL);
    }

    /* Build the full page: inject bridge before the modgui content */
    GString* page = g_string_new("<!DOCTYPE html><html><head>"
                                  "<meta charset='utf-8'/>");
    if (bridge_js) {
        g_string_append(page, "<script>");
        g_string_append(page, bridge_js);
        g_string_append(page, "</script>");
    }
    g_string_append(page, "</head><body>");
    g_string_append_len(page, html, (gssize)html_len);
    g_string_append(page, "</body></html>");

    webkit_web_view_load_html((WebKitWebView*)ui->webview,
                               page->str,
                               base_uri ? base_uri : "about:blank");

    g_string_free(page, TRUE);
    g_free(html);
    g_free(bridge_js);
    g_free(base_uri);
}

/* ── Plugin picker dialog ─────────────────────────────────────────────────── */

static void on_load_plugin_clicked(GtkButton* btn, gpointer user_data)
{
    (void)btn;
    ModguiHostUI* ui = (ModguiHostUI*)user_data;

    GtkWidget* dialog = gtk_dialog_new_with_buttons(
        "Select LV2 Plugin with ModGUI",
        GTK_WINDOW(gtk_widget_get_toplevel(ui->root)),
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT,
        "_Cancel", GTK_RESPONSE_CANCEL,
        "_Load",   GTK_RESPONSE_ACCEPT,
        NULL);

    gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 400);

    /* List store: name, uri */
    GtkListStore* store = gtk_list_store_new(2,
                                              G_TYPE_STRING,
                                              G_TYPE_STRING);

    for (int i = 0; i < ui->n_plugins; i++) {
        const PluginInfo* p = &ui->plugin_list[i];
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
                           0, p->name ? p->name : p->uri,
                           1, p->uri,
                           -1);
    }

    GtkWidget* tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    GtkTreeViewColumn* col;
    GtkCellRenderer*   cell;

    cell = gtk_cell_renderer_text_new();
    col  = gtk_tree_view_column_new_with_attributes("Name", cell,
                                                     "text", 0, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    cell = gtk_cell_renderer_text_new();
    col  = gtk_tree_view_column_new_with_attributes("URI", cell,
                                                     "text", 1, NULL);
    gtk_tree_view_column_set_expand(col, TRUE);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), col);

    gtk_tree_view_set_headers_visible(GTK_TREE_VIEW(tree), TRUE);

    GtkWidget* scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),
                                   GTK_POLICY_AUTOMATIC,
                                   GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), tree);

    GtkWidget* content = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_box_pack_start(GTK_BOX(content), scroll, TRUE, TRUE, 0);
    gtk_widget_show_all(dialog);

    gint response = gtk_dialog_run(GTK_DIALOG(dialog));
    if (response == GTK_RESPONSE_ACCEPT) {
        GtkTreeSelection* sel = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
        GtkTreeModel*     model;
        GtkTreeIter       iter;
        if (gtk_tree_selection_get_selected(sel, &model, &iter)) {
            gchar* name = NULL;
            gchar* uri  = NULL;
            gtk_tree_model_get(model, &iter, 0, &name, 1, &uri, -1);

            /* Find the full PluginInfo */
            const PluginInfo* chosen = NULL;
            for (int i = 0; i < ui->n_plugins; i++) {
                if (uri && strcmp(ui->plugin_list[i].uri, uri) == 0) {
                    chosen = &ui->plugin_list[i];
                    break;
                }
            }

            if (chosen) {
                /* Update label */
                gtk_label_set_text(GTK_LABEL(ui->plugin_label),
                                   chosen->name ? chosen->name : chosen->uri);

                /* Tell DSP about the new URI */
                send_hosted_uri(ui, chosen->uri);

                /* Load modgui in WebKit */
                load_modgui(ui, chosen);
            }

            g_free(name);
            g_free(uri);
        }
    }

    gtk_widget_destroy(dialog);
}

/* ── instantiate ─────────────────────────────────────────────────────────── */

static LV2UI_Handle
instantiate(const LV2UI_Descriptor*   descriptor,
            const char*                plugin_uri,
            const char*                bundle_path,
            LV2UI_Write_Function       write_function,
            LV2UI_Controller           controller,
            LV2UI_Widget*              widget,
            const LV2_Feature* const*  features)
{
    (void)descriptor; (void)plugin_uri;

    ModguiHostUI* ui = (ModguiHostUI*)calloc(1, sizeof(ModguiHostUI));
    if (!ui) return NULL;

    ui->write_function = write_function;
    ui->controller     = controller;

    const char* missing = lv2_features_query(features,
        LV2_URID__map, &ui->map, true,
        LV2_LOG__log,  &ui->logger.log, false,
        NULL);

    lv2_log_logger_set_map(&ui->logger, ui->map);

    if (missing) {
        lv2_log_error(&ui->logger,
                      "modgui-host UI: missing feature %s\n", missing);
        free(ui);
        return NULL;
    }

    LV2_URID_Map* map = ui->map;
    ui->urid.atom_Object       = map->map(map->handle, LV2_ATOM__Object);
    ui->urid.atom_String       = map->map(map->handle, LV2_ATOM__String);
    ui->urid.atom_Float        = map->map(map->handle, LV2_ATOM__Float);
    ui->urid.atom_Sequence     = map->map(map->handle, LV2_ATOM__Sequence);
    ui->urid.atom_event_transfer =
        map->map(map->handle, LV2_ATOM__eventTransfer);
    ui->urid.patch_Set         = map->map(map->handle, LV2_PATCH__Set);
    ui->urid.patch_property    = map->map(map->handle, LV2_PATCH__property);
    ui->urid.patch_value       = map->map(map->handle, LV2_PATCH__value);
    ui->urid.hosted_plugin_uri =
        map->map(map->handle, MODGUI_HOSTED_PLUGIN_URI);
    ui->urid.param_change      = map->map(map->handle, MODGUI_PARAM_CHANGE);
    ui->urid.param_symbol      = map->map(map->handle, MODGUI_PARAM_SYMBOL);
    ui->urid.param_value       = map->map(map->handle, MODGUI_PARAM_VALUE);

    lv2_atom_forge_init(&ui->forge, map);

    /* Bridge JS path: <bundle>/resources/bridge.js */
    ui->bridge_js_path =
        g_build_filename(bundle_path, "resources", "bridge.js", NULL);

    /* Lilv for plugin discovery */
    ui->world = lilv_world_new();
    lilv_world_load_all(ui->world);
    discover_plugins(ui);

    /* ── Build GTK UI ────────────────────────────────────────────────────── */
    /* Do NOT call gtk_init here — the host (Carla) already owns GTK init. */

    ui->root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_size_request(ui->root, 800, 600);

    /* Header bar */
    ui->header_bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(ui->header_bar, 6);
    gtk_widget_set_margin_end(ui->header_bar, 6);
    gtk_widget_set_margin_top(ui->header_bar, 4);
    gtk_widget_set_margin_bottom(ui->header_bar, 4);

    ui->plugin_label = gtk_label_new("No plugin loaded");
    gtk_label_set_ellipsize(GTK_LABEL(ui->plugin_label),
                             PANGO_ELLIPSIZE_END);
    gtk_widget_set_hexpand(ui->plugin_label, TRUE);
    gtk_label_set_xalign(GTK_LABEL(ui->plugin_label), 0.0f);

    ui->load_btn = gtk_button_new_with_label("Load Plugin…");
    g_signal_connect(ui->load_btn, "clicked",
                     G_CALLBACK(on_load_plugin_clicked), ui);

    gtk_box_pack_start(GTK_BOX(ui->header_bar), ui->plugin_label,
                        TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(ui->header_bar), ui->load_btn,
                     FALSE, FALSE, 0);

    /* Separator */
    GtkWidget* sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);

    /* WebKit web view
     * Disable the sandbox and hardware acceleration: WebKit's GPU/sandbox
     * subprocesses commonly crash when the plugin is embedded inside a host
     * process like Carla.  These settings keep everything in-process. */
    WebKitWebContext* wk_ctx = webkit_web_context_get_default();
    webkit_web_context_set_sandbox_enabled(wk_ctx, FALSE);

    WebKitSettings* wk_settings = webkit_settings_new();
    webkit_settings_set_hardware_acceleration_policy(
        wk_settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER);
    webkit_settings_set_enable_write_console_messages_to_stdout(
        wk_settings, TRUE);

    WebKitUserContentManager* mgr = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(mgr, "lv2");
    g_signal_connect(mgr, "script-message-received::lv2",
                     G_CALLBACK(on_script_message), ui);

    ui->webview = GTK_WIDGET(
        g_object_new(WEBKIT_TYPE_WEB_VIEW,
                     "user-content-manager", mgr,
                     "settings",             wk_settings,
                     NULL));
    g_object_unref(mgr);
    g_object_unref(wk_settings);

    gtk_widget_set_hexpand(ui->webview, TRUE);
    gtk_widget_set_vexpand(ui->webview, TRUE);

    /* Load placeholder page */
    webkit_web_view_load_html(
        (WebKitWebView*)ui->webview,
        "<html><body style='background:#1e1e1e;color:#ccc;"
        "display:flex;align-items:center;justify-content:center;"
        "height:100vh;font-family:sans-serif;font-size:18px'>"
        "<p>Click <b>Load Plugin…</b> to choose a plugin with ModGUI.</p>"
        "</body></html>",
        NULL);

    gtk_box_pack_start(GTK_BOX(ui->root), ui->header_bar, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ui->root), sep, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(ui->root), ui->webview, TRUE, TRUE, 0);

    gtk_widget_show_all(ui->root);

    *widget = (LV2UI_Widget)ui->root;
    return (LV2UI_Handle)ui;
}

/* ── cleanup ─────────────────────────────────────────────────────────────── */

static void
cleanup(LV2UI_Handle handle)
{
    ModguiHostUI* ui = (ModguiHostUI*)handle;
    if (ui->root) gtk_widget_destroy(ui->root);
    for (int i = 0; i < ui->n_plugins; i++)
        free_plugin_info(&ui->plugin_list[i]);
    free(ui->plugin_list);
    if (ui->world) lilv_world_free(ui->world);
    g_free(ui->bridge_js_path);
    free(ui);
}

/* ── port_event (DSP → UI) ───────────────────────────────────────────────── */

static void
port_event(LV2UI_Handle  handle,
           uint32_t      port_index,
           uint32_t      buffer_size,
           uint32_t      format,
           const void*   buffer)
{
    (void)buffer_size;
    ModguiHostUI* ui = (ModguiHostUI*)handle;

    if (port_index != PORT_EVENTS_OUT) return;
    if (format != ui->urid.atom_event_transfer) return;

    const LV2_Atom* atom = (const LV2_Atom*)buffer;
    if (atom->type != ui->urid.atom_Object) return;

    const LV2_Atom_Object* obj = (const LV2_Atom_Object*)atom;

    /* patch:Set with hosted plugin URI (sent back from DSP to confirm load) */
    if (obj->body.otype == ui->urid.patch_Set) {
        const LV2_Atom_URID* prop  = NULL;
        const LV2_Atom*      value = NULL;
        lv2_atom_object_get(obj,
            ui->urid.patch_property, &prop,
            ui->urid.patch_value,    &value,
            0);
        if (prop && value &&
            prop->body == ui->urid.hosted_plugin_uri &&
            value->type == ui->urid.atom_String) {
            /* DSP confirms which plugin is loaded — nothing extra needed */
        }
        return;
    }

    /* paramChange: DSP pushes a value update to the GUI */
    if (obj->body.otype == ui->urid.param_change) {
        const LV2_Atom_String* sym  = NULL;
        const LV2_Atom_Float*  fval = NULL;
        lv2_atom_object_get(obj,
            ui->urid.param_symbol, &sym,
            ui->urid.param_value,  &fval,
            0);
        if (sym && fval &&
            sym->atom.type  == ui->urid.atom_String &&
            fval->atom.type == ui->urid.atom_Float) {
            const char* symbol = (const char*)LV2_ATOM_BODY_CONST(sym);
            float       value  = fval->body;

            char script[512];
            snprintf(script, sizeof(script),
                     "if(window.lv2SetParameter)"
                     " window.lv2SetParameter('%s', %f);",
                     symbol, (double)value);
            run_js(ui, script);
        }
        return;
    }
}

/* ── Descriptor ──────────────────────────────────────────────────────────── */

static const LV2UI_Descriptor descriptor = {
    .URI            = MODGUI_HOST_UI_URI,
    .instantiate    = instantiate,
    .cleanup        = cleanup,
    .port_event     = port_event,
    .extension_data = NULL,
};

LV2_SYMBOL_EXPORT const LV2UI_Descriptor*
lv2ui_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
