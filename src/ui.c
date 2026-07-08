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
#include <lv2/midi/midi.h>

#include <lilv/lilv.h>

#include <gtk/gtk.h>

/* Both webkit2gtk-4.0 and webkit2gtk-4.1 use the same header path.
 * The WEBKIT_API_40 flag only controls which JS evaluation API to use. */
#include <webkit2/webkit2.h>

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

#include "common.h"

/* ── Types ───────────────────────────────────────────────────────────────── */

typedef struct {
    char*  name;           /* malloc-owned */
    char*  uri;            /* malloc-owned */
    gchar* bundle_path;    /* g_filename_from_uri, g_free */
    gchar* template_file;  /* g_filename_from_uri, g_free */
    gchar* resources_dir;  /* g_filename_from_uri, g_free */
    gchar* stylesheet;     /* g_filename_from_uri, g_free */
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
        LV2_URID patch_Get;
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

    /* Host resize API (optional — may be NULL) */
    LV2UI_Resize*       resize;
} ModguiHostUI;

/* ── Forward declarations ────────────────────────────────────────────────── */
static void     discover_plugins(ModguiHostUI* ui);
static void     free_plugin_info(PluginInfo* p);
static void     on_load_plugin_clicked(GtkButton* btn, gpointer user_data);
static void     on_row_activated(GtkTreeView* view, GtkTreePath* path,
                                  GtkTreeViewColumn* col, gpointer dialog_ptr);
static void     on_page_load_changed(WebKitWebView* view,
                                      WebKitLoadEvent event, gpointer user_data);
static void     load_modgui(ModguiHostUI* ui, const PluginInfo* p);
static void     send_hosted_uri(ModguiHostUI* ui, const char* uri);
static void     send_param_change(ModguiHostUI* ui,
                                   const char* symbol, float value);
static void     on_script_message(WebKitUserContentManager* mgr,
                                   WebKitJavascriptResult* result,
                                   gpointer user_data);
static gboolean run_js(ModguiHostUI* ui, const char* script);

/* ── Plugin list cache ───────────────────────────────────────────────────── */
/* Scanning all LV2 bundles via lilv_world_load_all() is slow.  We cache the
 * result in $XDG_CACHE_HOME/lv2-modgui/plugins.cache.  The cache is
 * invalidated if any of the standard LV2 directories has been modified more
 * recently than the cache file.
 *
 * Cache format: one plugin per line, six tab-separated fields:
 *   name TAB uri TAB bundle_path TAB template_file TAB resources_dir TAB stylesheet
 * Empty/NULL fields are stored as an empty string between tabs. */

static const char* plugin_cache_path(void)
{
    static gchar path[4096];
    if (!path[0])
        g_snprintf(path, sizeof(path), "%s/lv2-modgui/plugins.cache",
                   g_get_user_cache_dir());
    return path;
}

static gboolean lv2_dirs_newer_than(time_t t)
{
    static const char* const std[] = {
        "/usr/lib64/lv2", "/usr/lib/lv2",
        "/usr/local/lib64/lv2", "/usr/local/lib/lv2",
        NULL
    };
    struct stat st;
    for (int i = 0; std[i]; i++)
        if (stat(std[i], &st) == 0 && st.st_mtime > t) return TRUE;

    gchar* u = g_build_filename(g_get_home_dir(), ".lv2", NULL);
    gboolean newer = (stat(u, &st) == 0 && st.st_mtime > t);
    g_free(u);
    if (newer) return TRUE;

    const char* env = g_getenv("LV2_PATH");
    if (env) {
        gchar** dirs = g_strsplit(env, ":", -1);
        for (int i = 0; dirs[i]; i++)
            if (stat(dirs[i], &st) == 0 && st.st_mtime > t) {
                g_strfreev(dirs); return TRUE;
            }
        g_strfreev(dirs);
    }
    return FALSE;
}

static gboolean plugin_cache_valid(const char* path)
{
    struct stat st;
    return stat(path, &st) == 0 && !lv2_dirs_newer_than(st.st_mtime);
}

static void save_plugin_cache(const char* path, PluginInfo* list, int n)
{
    gchar* dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    g_free(dir);
    FILE* f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < n; i++)
        fprintf(f, "%s\t%s\t%s\t%s\t%s\t%s\n",
                list[i].name          ? list[i].name          : "",
                list[i].uri           ? list[i].uri           : "",
                list[i].bundle_path   ? list[i].bundle_path   : "",
                list[i].template_file ? list[i].template_file : "",
                list[i].resources_dir ? list[i].resources_dir : "",
                list[i].stylesheet    ? list[i].stylesheet    : "");
    fclose(f);
}

static gboolean load_plugin_cache(const char* path,
                                   PluginInfo** list_out, int* n_out)
{
    FILE* f = fopen(path, "r");
    if (!f) return FALSE;
    GArray* arr = g_array_new(FALSE, TRUE, sizeof(PluginInfo));
    gchar line[4096];
    while (fgets(line, sizeof(line), f)) {
        gsize len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        if (!len) continue;
        gchar** fld = g_strsplit(line, "\t", 6);
        int nf = 0; while (fld[nf]) nf++;
        if (nf < 6) { g_strfreev(fld); continue; }
        PluginInfo p = {0};
        /* name/uri freed with free() — use strdup to match discover_plugins */
        p.name          = fld[0][0] ? strdup(fld[0])   : NULL;
        p.uri           = fld[1][0] ? strdup(fld[1])   : NULL;
        /* rest freed with g_free() */
        p.bundle_path   = fld[2][0] ? g_strdup(fld[2]) : NULL;
        p.template_file = fld[3][0] ? g_strdup(fld[3]) : NULL;
        p.resources_dir = fld[4][0] ? g_strdup(fld[4]) : NULL;
        p.stylesheet    = fld[5][0] ? g_strdup(fld[5]) : NULL;
        g_strfreev(fld);
        if (!p.uri) { free(p.name); continue; }
        g_array_append_val(arr, p);
    }
    fclose(f);
    /* Copy to a calloc block to match the allocator used in discover_plugins */
    int n = (int)arr->len;
    PluginInfo* list = n ? (PluginInfo*)calloc((size_t)n, sizeof(PluginInfo))
                         : NULL;
    if (list) memcpy(list, arr->data, (size_t)n * sizeof(PluginInfo));
    g_array_free(arr, TRUE); /* frees GArray buffer; pointed-to strings survive */
    *list_out = list;
    *n_out    = list ? n : 0;
    return TRUE;
}

/* ── Plugin discovery ────────────────────────────────────────────────────── */

static void discover_plugins(ModguiHostUI* ui)
{
    ui->n_plugins   = 0;
    ui->plugin_list = NULL;

    /* Use the cache when possible to avoid a full LV2 scan on every open */
    const char* cpath = plugin_cache_path();
    if (plugin_cache_valid(cpath) &&
        load_plugin_cache(cpath, &ui->plugin_list, &ui->n_plugins)) {
        return;
    }

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

    /* Try iconTemplate first (current MOD convention), fall back to templateFile */
    LilvNode* icon_tmpl_node = lilv_new_uri(ui->world, MODGUI_ICON_TEMPLATE);
    LilvNode* tmpl_node      = lilv_new_uri(ui->world, MODGUI_TEMPLATE_FILE);
    LilvNode* res_dir_node   = lilv_new_uri(ui->world, MODGUI_RESOURCES_DIR);
    LilvNode* style_node     = lilv_new_uri(ui->world, MODGUI_STYLESHEET);

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

        /* modgui:gui value: either a blank node with properties, or a direct
         * URI pointing at the resources directory (older convention). */
        LilvNode* gui_node = lilv_nodes_get_first(gui_nodes);
        if (gui_node) {
            if (lilv_node_is_uri(gui_node)) {
                /* Direct URI form: <plugin> modgui:gui <modgui/> .
                 * The URI is the resources directory itself. */
                p->resources_dir =
                    g_filename_from_uri(lilv_node_as_uri(gui_node), NULL, NULL);
            } else {
                /* Blank-node form: modgui:gui [ modgui:iconTemplate <…> ; … ] */
                LilvNode* tmpl = lilv_world_get(ui->world, gui_node,
                                                 icon_tmpl_node, NULL);
                if (!tmpl)
                    tmpl = lilv_world_get(ui->world, gui_node,
                                          tmpl_node, NULL);
                if (tmpl) {
                    p->template_file =
                        g_filename_from_uri(lilv_node_as_uri(tmpl), NULL, NULL);
                    lilv_node_free(tmpl);
                }
                LilvNode* rdir = lilv_world_get(ui->world, gui_node,
                                                 res_dir_node, NULL);
                if (rdir) {
                    p->resources_dir =
                        g_filename_from_uri(lilv_node_as_uri(rdir), NULL, NULL);
                    lilv_node_free(rdir);
                }
                LilvNode* style = lilv_world_get(ui->world, gui_node,
                                                  style_node, NULL);
                if (style) {
                    p->stylesheet =
                        g_filename_from_uri(lilv_node_as_uri(style), NULL, NULL);
                    lilv_node_free(style);
                }
            }
        }

        /* Fallback: search for common template filenames in resources dir */
        if (!p->template_file && p->resources_dir) {
            static const char* const candidates[] = {
                "icon.html", "template.html", "index.html", NULL
            };
            for (int k = 0; candidates[k] && !p->template_file; k++) {
                gchar* path = g_build_filename(p->resources_dir,
                                               candidates[k], NULL);
                if (g_file_test(path, G_FILE_TEST_EXISTS))
                    p->template_file = path;
                else
                    g_free(path);
            }
        }

        lilv_nodes_free(gui_nodes);
    }

    lilv_node_free(modgui_gui);
    lilv_node_free(icon_tmpl_node);
    lilv_node_free(tmpl_node);
    lilv_node_free(res_dir_node);
    lilv_node_free(style_node);

    ui->n_plugins = idx;

    /* Persist the result so future opens skip the full scan */
    save_plugin_cache(cpath, ui->plugin_list, ui->n_plugins);
}

static void free_plugin_info(PluginInfo* p)
{
    if (!p) return;
    free(p->name);
    free(p->uri);
    /* bundle_path, template_file, resources_dir, stylesheet from g_filename_from_uri */
    g_free(p->bundle_path);
    g_free(p->template_file);
    g_free(p->resources_dir);
    g_free(p->stylesheet);
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
                  WebKitJavascriptResult*   result,
                  gpointer                  user_data)
{
    (void)mgr;
    ModguiHostUI* ui = (ModguiHostUI*)user_data;

    /* Both webkit2gtk-4.0 and -4.1 pass WebKitJavascriptResult* here.
       Only webkit2gtk-6.0 (GTK4) changed the signal to pass JSCValue* directly. */
    JSCValue* val = webkit_javascript_result_get_js_value(result);

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
            lv2_log_note(&ui->logger,
                         "modgui-host: parameterChange %s = %g\n", sym, dval);
            send_param_change(ui, sym, (float)dval);
            g_free(sym);
        }
    } else if (type_str && strcmp(type_str, "contentReady") == 0) {
        JSCValue* w_v = jsc_value_object_get_property(val, "width");
        JSCValue* h_v = jsc_value_object_get_property(val, "height");
        if (w_v && h_v &&
            jsc_value_is_number(w_v) && jsc_value_is_number(h_v)) {
            gint w = (gint)jsc_value_to_double(w_v);
            gint h = (gint)jsc_value_to_double(h_v);
            lv2_log_note(&ui->logger,
                         "modgui-host: contentReady w=%d h=%d\n", w, h);
            if (w > 0 && h > 0) {
                /* Compute header-bar + separator chrome from current layout */
                gint root_h = gtk_widget_get_allocated_height(ui->root);
                gint web_h  = gtk_widget_get_allocated_height(ui->webview);
                gint chrome = (root_h > web_h && web_h > 0)
                              ? (root_h - web_h) : 42;

                /* Stop the webview from expanding beyond content */
                gtk_widget_set_hexpand(ui->webview, FALSE);
                gtk_widget_set_vexpand(ui->webview, FALSE);
                gtk_widget_set_size_request(ui->webview, w, h);

                /* Lower the root's minimum size before resizing — otherwise
                 * the old minimum (e.g. 400px wide) prevents the window from
                 * shrinking to the plugin's actual width. */
                gtk_widget_set_size_request(ui->root, w, h + chrome);

                /* Notify the host so it can update its bookkeeping. */
                if (ui->resize) {
                    lv2_log_note(&ui->logger,
                                 "modgui-host: ui_resize(%d, %d)\n",
                                 w, h + chrome);
                    ui->resize->ui_resize(ui->resize->handle,
                                          w, h + chrome);
                }

                /* Force the GTK window to the exact content size.
                 * gtk_window_resize alone is not reliable: Carla's bridge
                 * may reset the width asynchronously via its own resize
                 * response.  gtk_window_set_resizable(FALSE) sends WM-level
                 * min=max=natural_size hints that no subsequent resize call
                 * can override.  The natural size is determined by
                 * set_size_request on the webview and root above. */
                GtkWidget* toplevel = gtk_widget_get_toplevel(ui->root);
                lv2_log_note(&ui->logger,
                             "modgui-host: set_resizable(FALSE) + resize(%d,%d) toplevel=%s\n",
                             w, h + chrome,
                             GTK_IS_WINDOW(toplevel) ? "GtkWindow" : "other");
                if (GTK_IS_WINDOW(toplevel)) {
                    gtk_window_set_resizable(GTK_WINDOW(toplevel), FALSE);
                    gtk_window_resize(GTK_WINDOW(toplevel), w, h + chrome);
                }
            }
        }
        if (w_v) g_object_unref(w_v);
        if (h_v) g_object_unref(h_v);
    }
    g_free(type_str);

cleanup:
    if (type_v)   g_object_unref(type_v);
    if (symbol_v) g_object_unref(symbol_v);
    if (value_v)  g_object_unref(value_v);
}

/* ── Atom helpers ────────────────────────────────────────────────────────── */

/* Ask the DSP for its current state (hosted plugin URI).
   Sent on UI open so that when the UI is reopened the DSP echoes the
   URI back via port_event and the plugin is automatically reloaded. */
static void request_state(ModguiHostUI* ui)
{
    lv2_atom_forge_set_buffer(&ui->forge, ui->forge_buf, sizeof(ui->forge_buf));
    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_object(&ui->forge, &frame, 0, ui->urid.patch_Get);
    lv2_atom_forge_pop(&ui->forge, &frame);
    LV2_Atom* atom = (LV2_Atom*)ui->forge_buf;
    ui->write_function(ui->controller, PORT_EVENTS_IN,
                       lv2_atom_total_size(atom),
                       ui->urid.atom_event_transfer,
                       atom);
}

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

/* ── Plugin JSON builder ─────────────────────────────────────────────────── */

static gchar* json_escape_string(const char* s)
{
    if (!s) return g_strdup("\"\"");
    GString* out = g_string_new("\"");
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        switch (*p) {
        case '"':  g_string_append(out, "\\\""); break;
        case '\\': g_string_append(out, "\\\\"); break;
        case '\n': g_string_append(out, "\\n");  break;
        case '\r': g_string_append(out, "\\r");  break;
        case '\t': g_string_append(out, "\\t");  break;
        default:
            if (*p < 0x20)
                g_string_append_printf(out, "\\u%04x", (unsigned)*p);
            else
                g_string_append_c(out, (char)*p);
            break;
        }
    }
    g_string_append_c(out, '"');
    return g_string_free(out, FALSE);
}

/* Helper for sorting modgui:port entries by lv2:index */
typedef struct { int idx; gchar* sym; gchar* name; } ModguiCtrlEntry;

static gint cmp_modgui_ctrl(gconstpointer a, gconstpointer b) {
    return ((const ModguiCtrlEntry*)a)->idx - ((const ModguiCtrlEntry*)b)->idx;
}

static gchar* build_plugin_json(LilvWorld* world,
                                 const char* plugin_uri,
                                 const char* plugin_name)
{
    LilvNode* uri_node = lilv_new_uri(world, plugin_uri);
    const LilvPlugin* lp =
        lilv_plugins_get_by_uri(lilv_world_get_all_plugins(world), uri_node);
    lilv_node_free(uri_node);
    if (!lp) return g_strdup("{}");

    LilvNode* audio_class   = lilv_new_uri(world, LV2_CORE__AudioPort);
    LilvNode* control_class = lilv_new_uri(world, LV2_CORE__ControlPort);
    LilvNode* input_class   = lilv_new_uri(world, LV2_CORE__InputPort);
    LilvNode* output_class  = lilv_new_uri(world, LV2_CORE__OutputPort);
    LilvNode* atom_class    = lilv_new_uri(world, LV2_ATOM__AtomPort);
    LilvNode* supports_node = lilv_new_uri(world, LV2_ATOM__supports);
    LilvNode* midi_event    = lilv_new_uri(world, LV2_MIDI__MidiEvent);
    LilvNode* lv2_min       = lilv_new_uri(world, LV2_CORE__minimum);
    LilvNode* lv2_max       = lilv_new_uri(world, LV2_CORE__maximum);
    LilvNode* lv2_int_prop  = lilv_new_uri(world, LV2_CORE__integer);
    /* modgui namespace — brand/label/color/knob/port */
#define MODGUI_NS "http://moddevices.com/ns/modgui#"
    LilvNode* mg_gui_pred   = lilv_new_uri(world, MODGUI_NS "gui");
    LilvNode* mg_brand_pred = lilv_new_uri(world, MODGUI_NS "brand");
    LilvNode* mg_label_pred = lilv_new_uri(world, MODGUI_NS "label");
    LilvNode* mg_color_pred = lilv_new_uri(world, MODGUI_NS "color");
    LilvNode* mg_knob_pred  = lilv_new_uri(world, MODGUI_NS "knob");
    LilvNode* mg_port_pred  = lilv_new_uri(world, MODGUI_NS "port");
    LilvNode* lv2_name_pred = lilv_new_uri(world, LV2_CORE__name);
    LilvNode* lv2_sym_pred  = lilv_new_uri(world, LV2_CORE__symbol);
    LilvNode* lv2_idx_pred  = lilv_new_uri(world,
                                            "http://lv2plug.in/ns/lv2core#index");

    uint32_t n_ports = lilv_plugin_get_num_ports(lp);
    float* defaults = (float*)calloc(n_ports, sizeof(float));
    lilv_plugin_get_port_ranges_float(lp, NULL, NULL, defaults);

    GString* audio_in  = g_string_new("[");
    GString* audio_out = g_string_new("[");
    GString* midi_in   = g_string_new("[");
    GString* midi_out  = g_string_new("[");
    GString* ctrl_in   = g_string_new("[");
    GString* ctrl_out  = g_string_new("[");
    bool first[6]      = {true, true, true, true, true, true};

    for (uint32_t i = 0; i < n_ports; i++) {
        const LilvPort* port = lilv_plugin_get_port_by_index(lp, i);

        bool is_in      = lilv_port_is_a(lp, port, input_class);
        bool is_audio   = lilv_port_is_a(lp, port, audio_class);
        bool is_control = lilv_port_is_a(lp, port, control_class);
        bool is_atom    = lilv_port_is_a(lp, port, atom_class);

        bool is_midi = false;
        if (is_atom) {
            LilvNodes* sup = lilv_port_get_value(lp, port, supports_node);
            if (sup) {
                is_midi = lilv_nodes_contains(sup, midi_event);
                lilv_nodes_free(sup);
            }
        }

        const LilvNode* sym_node  = lilv_port_get_symbol(lp, port);
        LilvNode*        name_node = lilv_port_get_name(lp, port);
        const char* sym   = sym_node  ? lilv_node_as_string(sym_node)  : "";
        const char* pname = name_node ? lilv_node_as_string(name_node) : sym;

        gchar* jsym  = json_escape_string(sym);
        gchar* jname = json_escape_string(pname);

        GString* arr  = NULL;
        int      fi   = -1;

        if (is_audio) {
            arr = is_in ? audio_in  : audio_out;
            fi  = is_in ? 0 : 1;
            if (!first[fi]) g_string_append_c(arr, ',');
            g_string_append_printf(arr,
                "{\"index\":%u,\"symbol\":%s,\"name\":%s}", i, jsym, jname);
        } else if (is_midi) {
            arr = is_in ? midi_in  : midi_out;
            fi  = is_in ? 2 : 3;
            if (!first[fi]) g_string_append_c(arr, ',');
            g_string_append_printf(arr,
                "{\"index\":%u,\"symbol\":%s,\"name\":%s}", i, jsym, jname);
        } else if (is_control) {
            arr = is_in ? ctrl_in  : ctrl_out;
            fi  = is_in ? 4 : 5;

            LilvNode* mn = lilv_port_get(lp, port, lv2_min);
            LilvNode* mx = lilv_port_get(lp, port, lv2_max);
            /* Accept both xsd:float and xsd:integer TTL literals */
#define NODE_AS_FLOAT(n, fallback) \
    ((n) ? (lilv_node_is_float(n) ? lilv_node_as_float(n) \
          : lilv_node_is_int(n)   ? (float)lilv_node_as_int(n) \
                                  : (fallback)) : (fallback))
            float min_v = NODE_AS_FLOAT(mn, 0.0f);
            float max_v = NODE_AS_FLOAT(mx, 1.0f);
#undef NODE_AS_FLOAT
            /* Sanitise: nan/inf are not valid JSON/JS tokens */
            if (!isfinite(min_v)) min_v = 0.0f;
            if (!isfinite(max_v)) max_v = 1.0f;
            float def_v = isfinite(defaults[i]) ? defaults[i] : min_v;
            bool  is_int_port = lilv_port_has_property(lp, port, lv2_int_prop);
            lilv_node_free(mn);
            lilv_node_free(mx);

            if (!first[fi]) g_string_append_c(arr, ',');
            g_string_append_printf(arr,
                "{\"index\":%u,\"symbol\":%s,\"name\":%s,"
                "\"minimum\":%.6g,\"maximum\":%.6g,\"default\":%.6g,"
                "\"value\":%.6g,\"integer\":%s}",
                i, jsym, jname,
                (double)min_v, (double)max_v, (double)def_v, (double)def_v,
                is_int_port ? "true" : "false");
        }

        if (fi >= 0) first[fi] = false;
        g_free(jsym);
        g_free(jname);
        lilv_node_free(name_node);
    }

    free(defaults);
    g_string_append_c(audio_in, ']'); g_string_append_c(audio_out, ']');
    g_string_append_c(midi_in,  ']'); g_string_append_c(midi_out,  ']');
    g_string_append_c(ctrl_in,  ']'); g_string_append_c(ctrl_out,  ']');

    /* Query modgui:gui blank node for brand/label/color/knob/controls */
    gchar* mg_brand_val = g_strdup("");
    gchar* mg_label_val = g_strdup(plugin_name ? plugin_name : "");
    gchar* mg_color_val = g_strdup("");
    gchar* mg_knob_val  = g_strdup("");
    GString* controls   = g_string_new("[");
    bool first_ctrl     = true;

    GArray* ctrl_arr  = g_array_new(FALSE, FALSE, sizeof(ModguiCtrlEntry));
    LilvNodes* gui_ns = lilv_plugin_get_value(lp, mg_gui_pred);
    if (gui_ns && lilv_nodes_size(gui_ns) > 0) {
        const LilvNode* gn = lilv_nodes_get_first(gui_ns);
        LilvNode* v;
        if ((v = lilv_world_get(world, gn, mg_brand_pred, NULL))) {
            g_free(mg_brand_val);
            mg_brand_val = g_strdup(lilv_node_as_string(v));
            lilv_node_free(v);
        }
        if ((v = lilv_world_get(world, gn, mg_label_pred, NULL))) {
            g_free(mg_label_val);
            mg_label_val = g_strdup(lilv_node_as_string(v));
            lilv_node_free(v);
        }
        if ((v = lilv_world_get(world, gn, mg_color_pred, NULL))) {
            g_free(mg_color_val);
            mg_color_val = g_strdup(lilv_node_as_string(v));
            lilv_node_free(v);
        }
        if ((v = lilv_world_get(world, gn, mg_knob_pred, NULL))) {
            g_free(mg_knob_val);
            mg_knob_val = g_strdup(lilv_node_as_string(v));
            lilv_node_free(v);
        }
        LilvNodes* pns = lilv_world_find_nodes(world, gn, mg_port_pred, NULL);
        if (pns) {
            LILV_FOREACH(nodes, pi, pns) {
                const LilvNode* pn = lilv_nodes_get(pns, pi);
                LilvNode* iv = lilv_world_get(world, pn, lv2_idx_pred,  NULL);
                LilvNode* sv = lilv_world_get(world, pn, lv2_sym_pred,  NULL);
                LilvNode* nv = lilv_world_get(world, pn, lv2_name_pred, NULL);
                if (sv) {
                    ModguiCtrlEntry ce;
                    ce.idx  = (iv && lilv_node_is_int(iv))
                              ? lilv_node_as_int(iv) : 9999;
                    ce.sym  = g_strdup(lilv_node_as_string(sv));
                    ce.name = g_strdup(nv ? lilv_node_as_string(nv)
                                          : lilv_node_as_string(sv));
                    g_array_append_val(ctrl_arr, ce);
                }
                if (iv) lilv_node_free(iv);
                if (sv) lilv_node_free(sv);
                if (nv) lilv_node_free(nv);
            }
            lilv_nodes_free(pns);
        }
    }
    if (gui_ns) lilv_nodes_free(gui_ns);

    g_array_sort(ctrl_arr, cmp_modgui_ctrl);
    for (guint i = 0; i < ctrl_arr->len; i++) {
        ModguiCtrlEntry* ce = &g_array_index(ctrl_arr, ModguiCtrlEntry, i);
        gchar* js = json_escape_string(ce->sym);
        gchar* jn = json_escape_string(ce->name);
        if (!first_ctrl) g_string_append_c(controls, ',');
        g_string_append_printf(controls, "{\"symbol\":%s,\"name\":%s}", js, jn);
        first_ctrl = false;
        g_free(js); g_free(jn);
        g_free(ce->sym); g_free(ce->name);
    }
    g_array_free(ctrl_arr, TRUE);
    g_string_append_c(controls, ']');

    gchar* juri    = json_escape_string(plugin_uri);
    gchar* jname   = json_escape_string(plugin_name);
    gchar* jbrand  = json_escape_string(mg_brand_val);
    gchar* jlabel  = json_escape_string(mg_label_val);
    gchar* jcolor  = json_escape_string(mg_color_val);
    gchar* jknob   = json_escape_string(mg_knob_val);
    gchar* result  = g_strdup_printf(
        "{"
          "\"effect\":{"
            "\"uri\":%s,\"name\":%s,"
            "\"ports\":{"
              "\"audio\":{\"input\":%s,\"output\":%s},"
              "\"midi\":{\"input\":%s,\"output\":%s},"
              "\"control\":{\"input\":%s,\"output\":%s}"
            "}"
          "},"
          "\"brand\":%s,"
          "\"label\":%s,"
          "\"color\":%s,"
          "\"knob\":%s,"
          "\"controls\":%s"
        "}",
        juri, jname,
        audio_in->str, audio_out->str,
        midi_in->str,  midi_out->str,
        ctrl_in->str,  ctrl_out->str,
        jbrand, jlabel, jcolor, jknob,
        controls->str);

    g_free(juri);  g_free(jname);
    g_free(jbrand); g_free(jlabel); g_free(jcolor); g_free(jknob);
    g_free(mg_brand_val); g_free(mg_label_val);
    g_free(mg_color_val); g_free(mg_knob_val);
    g_string_free(controls, TRUE);
    g_string_free(audio_in, TRUE); g_string_free(audio_out, TRUE);
    g_string_free(midi_in,  TRUE); g_string_free(midi_out,  TRUE);
    g_string_free(ctrl_in,  TRUE); g_string_free(ctrl_out,  TRUE);
    lilv_node_free(audio_class);  lilv_node_free(control_class);
    lilv_node_free(input_class);  lilv_node_free(output_class);
    lilv_node_free(atom_class);   lilv_node_free(supports_node);
    lilv_node_free(midi_event);   lilv_node_free(lv2_min);
    lilv_node_free(lv2_max);      lilv_node_free(lv2_int_prop);
    lilv_node_free(mg_gui_pred);  lilv_node_free(mg_brand_pred);
    lilv_node_free(mg_label_pred); lilv_node_free(mg_color_pred);
    lilv_node_free(mg_knob_pred); lilv_node_free(mg_port_pred);
    lilv_node_free(lv2_name_pred); lilv_node_free(lv2_sym_pred);
    lilv_node_free(lv2_idx_pred);

    return result;
}

/* ── CSS preprocessing helpers ───────────────────────────────────────────── */

static gchar* str_replace_all(const gchar* src,
                               const gchar* find,
                               const gchar* replace)
{
    gchar** parts = g_strsplit(src, find, -1);
    gchar*  result = g_strjoinv(replace, parts);
    g_strfreev(parts);
    return result;
}

/* Prepare a modgui CSS string for local use:
 *   1. Strip MOD server-side {{{ns}}} cache-busting tokens.
 *   2. Rewrite absolute /resources/ URL prefixes to bare filenames so they
 *      resolve against the base_uri (the plugin's modgui directory). */
static gchar* preprocess_css(const gchar* css)
{
    gchar* s1 = str_replace_all(css, "{{{ns}}}", "");
    gchar* s2 = str_replace_all(s1, "{{{cns}}}", "");
    gchar* s3 = str_replace_all(s2, "url(/resources/", "url(");
    g_free(s1);
    g_free(s2);
    return s3;
}

/* ── Load modgui into WebKit ─────────────────────────────────────────────── */

static gboolean on_load_failed(WebKitWebView*  webview,
                                WebKitLoadEvent event,
                                const gchar*    uri,
                                GError*         error,
                                gpointer        user_data)
{
    (void)webview; (void)event;
    ModguiHostUI* ui = (ModguiHostUI*)user_data;
    lv2_log_error(&ui->logger,
                  "modgui-host: WebKit failed to load '%s': %s\n",
                  uri ? uri : "(null)",
                  error ? error->message : "unknown error");
    return FALSE;
}

static void load_modgui(ModguiHostUI* ui, const PluginInfo* p)
{
    if (!p->template_file) {
        lv2_log_warning(&ui->logger,
                        "modgui-host: plugin '%s' has no iconTemplate or "
                        "templateFile in its modgui:gui node\n",
                        p->name ? p->name : p->uri);
        return;
    }

    lv2_log_note(&ui->logger,
                 "modgui-host: loading template %s\n", p->template_file);

    /* Read template HTML */
    gchar*  html     = NULL;
    gsize   html_len = 0;
    GError* err      = NULL;
    if (!g_file_get_contents(p->template_file, &html, &html_len, &err)) {
        lv2_log_error(&ui->logger,
                      "modgui-host: cannot read '%s': %s\n",
                      p->template_file, err ? err->message : "?");
        g_clear_error(&err);
        return;
    }

    /* Build base URI from the resources directory (or bundle).
     * Must end with '/' so that relative paths (img, css, js) resolve
     * relative to the directory, not its parent. */
    const gchar* base_dir = p->resources_dir ? p->resources_dir
                                              : p->bundle_path;
    gchar* base_uri = NULL;
    if (base_dir) {
        gchar* dir_with_slash = g_str_has_suffix(base_dir, "/")
                                ? g_strdup(base_dir)
                                : g_strconcat(base_dir, "/", NULL);
        base_uri = g_filename_to_uri(dir_with_slash, NULL, NULL);
        g_free(dir_with_slash);
    }

    lv2_log_note(&ui->logger,
                 "modgui-host: base URI %s\n",
                 base_uri ? base_uri : "(none)");

    /* Build plugin data JSON for Handlebars template rendering */
    gchar* plugin_json = build_plugin_json(ui->world, p->uri, p->name);

    /* Prepare bridge script */
    gchar* bridge_js = NULL;
    if (ui->bridge_js_path) {
        gsize blen = 0;
        g_file_get_contents(ui->bridge_js_path, &bridge_js, &blen, NULL);
    }

    /* Build page: always wrap the template in a minimal shell so we can
     * inject __MOD_DATA__ and bridge.js reliably in <head>.
     * bridge.js renders the Handlebars template on DOMContentLoaded. */
    GString* page = g_string_new(
        "<!DOCTYPE html><html><head><meta charset='utf-8'/>");

    /* 1. Plugin data (must come before bridge.js) */
    if (plugin_json) {
        g_string_append(page, "<script>window.__MOD_DATA__=");
        g_string_append(page, plugin_json);
        g_string_append(page, ";</script>");
    }

    /* 2. Bridge + mini Handlebars */
    if (bridge_js) {
        g_string_append(page, "<script>");
        g_string_append(page, bridge_js);
        g_string_append(page, "</script>");
    }

    /* 3. Plugin stylesheet (preprocessed: strip {{{ns}}}, rewrite /resources/) */
    if (p->stylesheet) {
        gchar* raw_css = NULL;
        gsize  css_len = 0;
        if (g_file_get_contents(p->stylesheet, &raw_css, &css_len, NULL)) {
            gchar* css = preprocess_css(raw_css);
            g_free(raw_css);
            g_string_append(page, "<style>");
            g_string_append(page, css);
            g_string_append(page, "</style>");
            g_free(css);
        }
    }

    /* 4. Global overrides: disable the MOD drag-handle overlay so it never
     *    swallows pointer events, strip default body margin/scroll, and fix
     *    the pedal background.  overflow:hidden on .mod-pedal creates a BFC
     *    which prevents the first in-flow child's top margin from collapsing
     *    through to the parent — without it, plugins that position control
     *    groups via margin-top (e.g. GxHarmonizer) place them at y=0 inside
     *    the pedal instead of the intended offset, misaligning knobs with the
     *    background image.  background-position:top anchors the image when
     *    overflow content would otherwise drift a centered background. */
    g_string_append(page,
        "<style>"
        "[mod-role=\"drag-handle\"],.mod-drag-handle"
            "{pointer-events:none!important}"
        "body{margin:0;padding:0;overflow:hidden}"
        ".mod-pedal{background-position:top center;overflow:hidden}"
        "</style>");

    g_string_append(page, "</head><body>");
    g_string_append_len(page, html, (gssize)html_len);
    g_string_append(page, "</body></html>");

    /* Connect load-failed signal once (guard against double-connect) */
    if (!g_signal_handler_find(ui->webview,
                                G_SIGNAL_MATCH_FUNC, 0, 0, NULL,
                                G_CALLBACK(on_load_failed), ui)) {
        g_signal_connect(ui->webview, "load-failed",
                         G_CALLBACK(on_load_failed), ui);
    }

    webkit_web_view_load_html((WebKitWebView*)ui->webview,
                               page->str,
                               base_uri ? base_uri : "about:blank");

    g_string_free(page, TRUE);
    g_free(html);
    g_free(bridge_js);
    g_free(plugin_json);
    g_free(base_uri);
}

/* ── WebKit load callback ────────────────────────────────────────────────── */

static void on_page_load_changed(WebKitWebView* view, WebKitLoadEvent event,
                                   gpointer user_data)
{
    (void)user_data;
    /* Grab GTK focus so WebKit receives keyboard/pointer events when embedded
     * in a host like Carla that may not automatically focus the plugin view. */
    if (event == WEBKIT_LOAD_FINISHED)
        gtk_widget_grab_focus(GTK_WIDGET(view));
}

/* ── Plugin picker dialog ─────────────────────────────────────────────────── */

static void on_row_activated(GtkTreeView* view, GtkTreePath* path,
                               GtkTreeViewColumn* col, gpointer dialog_ptr)
{
    (void)view; (void)path; (void)col;
    gtk_dialog_response(GTK_DIALOG(dialog_ptr), GTK_RESPONSE_ACCEPT);
}

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

    /* Double-click on a row immediately accepts the dialog */
    g_signal_connect(tree, "row-activated",
                     G_CALLBACK(on_row_activated), dialog);

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
            PluginInfo* chosen = NULL;
            for (int i = 0; i < ui->n_plugins; i++) {
                if (uri && strcmp(ui->plugin_list[i].uri, uri) == 0) {
                    chosen = &ui->plugin_list[i];
                    break;
                }
            }

            if (chosen) {
                ui->current_plugin = chosen;

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
    ui->urid.patch_Get         = map->map(map->handle, LV2_PATCH__Get);
    ui->urid.patch_Set         = map->map(map->handle, LV2_PATCH__Set);
    ui->urid.patch_property    = map->map(map->handle, LV2_PATCH__property);
    ui->urid.patch_value       = map->map(map->handle, LV2_PATCH__value);
    ui->urid.hosted_plugin_uri =
        map->map(map->handle, MODGUI_HOSTED_PLUGIN_URI);
    ui->urid.param_change      = map->map(map->handle, MODGUI_PARAM_CHANGE);
    ui->urid.param_symbol      = map->map(map->handle, MODGUI_PARAM_SYMBOL);
    ui->urid.param_value       = map->map(map->handle, MODGUI_PARAM_VALUE);

    lv2_atom_forge_init(&ui->forge, map);

    /* Optional: host resize API (lets us request window size changes) */
    for (const LV2_Feature* const* f = features; *f; ++f) {
        if (strcmp((*f)->URI, LV2_UI__resize) == 0) {
            ui->resize = (LV2UI_Resize*)(*f)->data;
            break;
        }
    }

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
    gtk_widget_set_size_request(ui->root, 400, 200);

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

    /* Ensure WebKit can receive focus in embedded hosts (e.g. Carla).
     * Without explicit focus the widget may not deliver mouse events to JS. */
    gtk_widget_set_can_focus(ui->webview, TRUE);
    gtk_widget_add_events(ui->webview,
        GDK_BUTTON_PRESS_MASK   | GDK_BUTTON_RELEASE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK         |
        GDK_KEY_PRESS_MASK      | GDK_FOCUS_CHANGE_MASK);
    g_signal_connect(ui->webview, "load-changed",
                     G_CALLBACK(on_page_load_changed), ui);

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

    /* Ask DSP for its current hosted plugin URI so that if the UI is reopened
       after a previous session the plugin is automatically reloaded. */
    request_state(ui);

    *widget = (LV2UI_Widget)ui->root;
    return (LV2UI_Handle)ui;
}

/* ── cleanup ─────────────────────────────────────────────────────────────── */

static void
cleanup(LV2UI_Handle handle)
{
    ModguiHostUI* ui = (ModguiHostUI*)handle;
    /* ui->root is owned by the host after we returned it as LV2UI_Widget;
       the host destroys it — calling gtk_widget_destroy here double-frees. */
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
            const char* uri_str = (const char*)LV2_ATOM_BODY_CONST(value);
            /* Only restore if different from what's already showing */
            if (!ui->current_plugin ||
                strcmp(ui->current_plugin->uri, uri_str) != 0) {
                for (int i = 0; i < ui->n_plugins; i++) {
                    if (strcmp(ui->plugin_list[i].uri, uri_str) == 0) {
                        ui->current_plugin = &ui->plugin_list[i];
                        gtk_label_set_text(
                            GTK_LABEL(ui->plugin_label),
                            ui->plugin_list[i].name
                                ? ui->plugin_list[i].name : uri_str);
                        load_modgui(ui, &ui->plugin_list[i]);
                        break;
                    }
                }
            }
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
