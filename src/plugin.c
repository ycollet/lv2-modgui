#include <lv2/core/lv2.h>
#include <lv2/core/lv2_util.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/util.h>
#include <lv2/atom/forge.h>
#include <lv2/urid/urid.h>
#include <lv2/state/state.h>
#include <lv2/patch/patch.h>
#include <lv2/log/log.h>
#include <lv2/log/logger.h>
#include <lv2/worker/worker.h>

#include <lilv/lilv.h>

#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "common.h"

/* ── Types ───────────────────────────────────────────────────────────────── */

typedef struct {
    char*    symbol;
    uint32_t port_index;   /* index in the hosted plugin */
    float    value;
    float    min;
    float    max;
    float    def;
    bool     is_input;
} ControlPort;

/* Data exchanged between work() and work_response() */
typedef struct {
    LilvInstance*      instance;
    const LilvPlugin*  lp;          /* non-owning ref into LilvWorld */
    uint32_t           n_ports;     /* total port count of hosted plugin */
    ControlPort*       ctrl_ports;
    int                n_ctrl_ports;
    float*             ctrl_buffers;  /* one float per ctrl port, owned */
    /* audio port indices in the hosted plugin (-1 = absent) */
    int ai_l, ai_r, ao_l, ao_r;
    char*              uri;           /* owned: the plugin URI that was loaded */
} HostedData;

typedef struct {
    /* LV2 features */
    LV2_URID_Map*        map;
    LV2_Worker_Schedule* schedule;
    LV2_Log_Logger      logger;

    struct {
        LV2_URID atom_Float;
        LV2_URID atom_Int;
        LV2_URID atom_Object;
        LV2_URID atom_String;
        LV2_URID atom_URID;
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

    /* Wrapper ports */
    const float*             audio_in_l;
    const float*             audio_in_r;
    float*                   audio_out_l;
    float*                   audio_out_r;
    const LV2_Atom_Sequence* events_in;
    LV2_Atom_Sequence*       events_out;

    /* Atom forge for events_out */
    LV2_Atom_Forge forge;

    /* Hosted plugin state (swapped atomically in work_response) */
    HostedData hosted;

    /* Current hosted plugin URI (for state save) */
    char* hosted_plugin_uri;

    /* Lilv world (loaded once at instantiate) */
    LilvWorld* world;

    /* Scratch buffers for ports we must connect but don't use */
    float dummy_float;
    float dummy_audio[MAX_BLOCK_SIZE];
    uint8_t dummy_atom[ATOM_BUF_SIZE];

    double sample_rate;

    /* Set by work_response() to make run() push the loaded URI to the UI */
    bool send_uri_to_ui;
} ModguiHost;

/* ── Forward declarations ────────────────────────────────────────────────── */
static bool load_hosted_plugin(ModguiHost* plugin, const char* uri,
                                HostedData* out);
static void free_hosted_data(HostedData* d);
static void connect_hosted_ports(ModguiHost* plugin, HostedData* d);
static void set_control_by_symbol(HostedData* d,
                                   const char* symbol, float value);

/* ── instantiate ─────────────────────────────────────────────────────────── */

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                     rate,
            const char*                bundle_path,
            const LV2_Feature* const*  features)
{
    (void)descriptor; (void)bundle_path;

    ModguiHost* plugin = (ModguiHost*)calloc(1, sizeof(ModguiHost));
    if (!plugin) return NULL;

    plugin->sample_rate = rate;
    plugin->hosted.ai_l = plugin->hosted.ai_r = -1;
    plugin->hosted.ao_l = plugin->hosted.ao_r = -1;

    const char* missing = lv2_features_query(features,
        LV2_URID__map,       &plugin->map,      true,
        LV2_WORKER__schedule, &plugin->schedule, true,
        LV2_LOG__log,        &plugin->logger.log, false,
        NULL);

    lv2_log_logger_set_map(&plugin->logger, plugin->map);

    if (missing) {
        lv2_log_error(&plugin->logger,
                      "modgui-host: missing required feature %s\n", missing);
        free(plugin);
        return NULL;
    }

    LV2_URID_Map* map = plugin->map;

    plugin->urid.atom_Float        = map->map(map->handle, LV2_ATOM__Float);
    plugin->urid.atom_Int          = map->map(map->handle, LV2_ATOM__Int);
    plugin->urid.atom_Object       = map->map(map->handle, LV2_ATOM__Object);
    plugin->urid.atom_String       = map->map(map->handle, LV2_ATOM__String);
    plugin->urid.atom_URID         = map->map(map->handle, LV2_ATOM__URID);
    plugin->urid.atom_Sequence     = map->map(map->handle, LV2_ATOM__Sequence);
    plugin->urid.atom_event_transfer =
        map->map(map->handle, LV2_ATOM__eventTransfer);
    plugin->urid.patch_Set         = map->map(map->handle, LV2_PATCH__Set);
    plugin->urid.patch_property    = map->map(map->handle, LV2_PATCH__property);
    plugin->urid.patch_value       = map->map(map->handle, LV2_PATCH__value);
    plugin->urid.hosted_plugin_uri =
        map->map(map->handle, MODGUI_HOSTED_PLUGIN_URI);
    plugin->urid.param_change      =
        map->map(map->handle, MODGUI_PARAM_CHANGE);
    plugin->urid.param_symbol      =
        map->map(map->handle, MODGUI_PARAM_SYMBOL);
    plugin->urid.param_value       =
        map->map(map->handle, MODGUI_PARAM_VALUE);

    lv2_atom_forge_init(&plugin->forge, map);

    plugin->world = lilv_world_new();
    lilv_world_load_all(plugin->world);

    /* Initialise dummy atom port as empty sequence */
    LV2_Atom_Sequence* dummy_seq = (LV2_Atom_Sequence*)plugin->dummy_atom;
    dummy_seq->atom.type = plugin->urid.atom_Sequence;
    dummy_seq->atom.size = sizeof(LV2_Atom_Sequence_Body);
    dummy_seq->body.unit = 0;
    dummy_seq->body.pad  = 0;

    return (LV2_Handle)plugin;
}

/* ── connect_port ────────────────────────────────────────────────────────── */

static void
connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    ModguiHost* plugin = (ModguiHost*)instance;
    switch ((enum ModguiPortIndex)port) {
    case PORT_AUDIO_IN_L:  plugin->audio_in_l  = (const float*)data; break;
    case PORT_AUDIO_IN_R:  plugin->audio_in_r  = (const float*)data; break;
    case PORT_AUDIO_OUT_L: plugin->audio_out_l = (float*)data; break;
    case PORT_AUDIO_OUT_R: plugin->audio_out_r = (float*)data; break;
    case PORT_EVENTS_IN:
        plugin->events_in  = (const LV2_Atom_Sequence*)data; break;
    case PORT_EVENTS_OUT:
        plugin->events_out = (LV2_Atom_Sequence*)data; break;
    default: break;
    }
}

/* ── activate / deactivate ───────────────────────────────────────────────── */

static void activate(LV2_Handle instance)
{
    ModguiHost* plugin = (ModguiHost*)instance;
    if (plugin->hosted.instance)
        lilv_instance_activate(plugin->hosted.instance);
}

static void deactivate(LV2_Handle instance)
{
    ModguiHost* plugin = (ModguiHost*)instance;
    if (plugin->hosted.instance)
        lilv_instance_deactivate(plugin->hosted.instance);
}

/* ── Helpers ─────────────────────────────────────────────────────────────── */

static void set_control_by_symbol(HostedData* d,
                                   const char* symbol, float value)
{
    for (int i = 0; i < d->n_ctrl_ports; i++) {
        if (strcmp(d->ctrl_ports[i].symbol, symbol) == 0) {
            float clamped = fmaxf(d->ctrl_ports[i].min,
                                  fminf(d->ctrl_ports[i].max, value));
            d->ctrl_buffers[i] = clamped;
            d->ctrl_ports[i].value = clamped;
            return;
        }
    }
}

/* ── run ─────────────────────────────────────────────────────────────────── */

static void run(LV2_Handle instance, uint32_t n_samples)
{
    ModguiHost* plugin = (ModguiHost*)instance;

    /* Prepare events_out as empty sequence */
    lv2_atom_forge_set_buffer(&plugin->forge,
                               (uint8_t*)plugin->events_out,
                               plugin->events_out
                                   ? sizeof(*plugin->events_out) + ATOM_BUF_SIZE
                                   : 0);
    LV2_Atom_Forge_Frame seq_frame;
    if (plugin->events_out)
        lv2_atom_forge_sequence_head(&plugin->forge, &seq_frame, 0);

    /* Process incoming atoms */
    if (plugin->events_in) {
        LV2_ATOM_SEQUENCE_FOREACH(plugin->events_in, ev) {
            if (ev->body.type != plugin->urid.atom_Object) continue;
            const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;

            /* patch:Set for hosted plugin URI */
            if (obj->body.otype == plugin->urid.patch_Set) {
                const LV2_Atom_URID* prop = NULL;
                const LV2_Atom*      val  = NULL;
                lv2_atom_object_get(obj,
                    plugin->urid.patch_property, &prop,
                    plugin->urid.patch_value,    &val,
                    0);
                if (prop && val &&
                    prop->body == plugin->urid.hosted_plugin_uri &&
                    val->type  == plugin->urid.atom_String) {
                    const char* uri =
                        (const char*)LV2_ATOM_BODY_CONST(val);
                    /* Schedule load on worker thread */
                    plugin->schedule->schedule_work(
                        plugin->schedule->handle,
                        strlen(uri) + 1, uri);
                }
                continue;
            }

            /* modgui-host:paramChange — control value from UI */
            if (obj->body.otype == plugin->urid.param_change) {
                const LV2_Atom_String* sym = NULL;
                const LV2_Atom_Float*  fval = NULL;
                lv2_atom_object_get(obj,
                    plugin->urid.param_symbol, &sym,
                    plugin->urid.param_value,  &fval,
                    0);
                if (sym && fval &&
                    sym->atom.type  == plugin->urid.atom_String &&
                    fval->atom.type == plugin->urid.atom_Float) {
                    set_control_by_symbol(&plugin->hosted,
                                          (const char*)LV2_ATOM_BODY_CONST(sym),
                                          fval->body);
                }
                continue;
            }
        }
    }

    /* Reconnect audio ports each cycle (host may call connect_port any time) */
    if (plugin->hosted.instance) {
        /* Copy our audio input to hosted plugin's audio inputs */
        if (plugin->hosted.ai_l >= 0 && plugin->audio_in_l)
            lilv_instance_connect_port(plugin->hosted.instance,
                                       (uint32_t)plugin->hosted.ai_l,
                                       (void*)plugin->audio_in_l);
        if (plugin->hosted.ai_r >= 0 && plugin->audio_in_r)
            lilv_instance_connect_port(plugin->hosted.instance,
                                       (uint32_t)plugin->hosted.ai_r,
                                       (void*)plugin->audio_in_r);

        /* Hosted plugin outputs go to our output (or dummy) */
        float* out_l = plugin->audio_out_l ? plugin->audio_out_l
                                           : plugin->dummy_audio;
        float* out_r = plugin->audio_out_r ? plugin->audio_out_r
                                           : plugin->dummy_audio;

        if (plugin->hosted.ao_l >= 0)
            lilv_instance_connect_port(plugin->hosted.instance,
                                       (uint32_t)plugin->hosted.ao_l, out_l);
        if (plugin->hosted.ao_r >= 0)
            lilv_instance_connect_port(plugin->hosted.instance,
                                       (uint32_t)plugin->hosted.ao_r, out_r);

        /* Handle mono output → stereo */
        if (plugin->hosted.ao_l >= 0 && plugin->hosted.ao_r < 0) {
            /* Mono plugin: duplicate L to R after run */
        }

        lilv_instance_run(plugin->hosted.instance, n_samples);

        /* Duplicate mono output if needed */
        if (plugin->hosted.ao_l >= 0 && plugin->hosted.ao_r < 0 &&
            plugin->audio_out_l && plugin->audio_out_r) {
            memcpy(plugin->audio_out_r, plugin->audio_out_l,
                   n_samples * sizeof(float));
        }
        /* If no output ports at all, pass through */
        if (plugin->hosted.ao_l < 0 && plugin->hosted.ao_r < 0) {
            if (plugin->audio_in_l && plugin->audio_out_l)
                memcpy(plugin->audio_out_l, plugin->audio_in_l,
                       n_samples * sizeof(float));
            if (plugin->audio_in_r && plugin->audio_out_r)
                memcpy(plugin->audio_out_r, plugin->audio_in_r,
                       n_samples * sizeof(float));
        }
    } else {
        /* No hosted plugin: pass through */
        if (plugin->audio_in_l && plugin->audio_out_l)
            memcpy(plugin->audio_out_l, plugin->audio_in_l,
                   n_samples * sizeof(float));
        if (plugin->audio_in_r && plugin->audio_out_r)
            memcpy(plugin->audio_out_r, plugin->audio_in_r,
                   n_samples * sizeof(float));
    }

    /* Notify UI of newly loaded plugin URI (set by work_response) */
    if (plugin->send_uri_to_ui && plugin->hosted_plugin_uri && plugin->events_out) {
        plugin->send_uri_to_ui = false;
        LV2_Atom_Forge_Frame obj_frame;
        lv2_atom_forge_frame_time(&plugin->forge, 0);
        lv2_atom_forge_object(&plugin->forge, &obj_frame, 0,
                              plugin->urid.patch_Set);
        lv2_atom_forge_key(&plugin->forge, plugin->urid.patch_property);
        lv2_atom_forge_urid(&plugin->forge, plugin->urid.hosted_plugin_uri);
        lv2_atom_forge_key(&plugin->forge, plugin->urid.patch_value);
        lv2_atom_forge_string(&plugin->forge,
                              plugin->hosted_plugin_uri,
                              strlen(plugin->hosted_plugin_uri));
        lv2_atom_forge_pop(&plugin->forge, &obj_frame);
    }

    if (plugin->events_out)
        lv2_atom_forge_pop(&plugin->forge, &seq_frame);
}

/* ── cleanup ─────────────────────────────────────────────────────────────── */

static void cleanup(LV2_Handle instance)
{
    ModguiHost* plugin = (ModguiHost*)instance;
    /* free_hosted_data handles deactivate + free — don't do it separately */
    free_hosted_data(&plugin->hosted);
    free(plugin->hosted_plugin_uri);
    if (plugin->world)
        lilv_world_free(plugin->world);
    free(plugin);
}

/* ── Worker ──────────────────────────────────────────────────────────────── */

static bool load_hosted_plugin(ModguiHost* plugin, const char* uri,
                                HostedData* out)
{
    memset(out, 0, sizeof(*out));
    out->ai_l = out->ai_r = out->ao_l = out->ao_r = -1;

    LilvNode* uri_node = lilv_new_uri(plugin->world, uri);
    if (!uri_node) return false;

    const LilvPlugins* all = lilv_world_get_all_plugins(plugin->world);
    const LilvPlugin*  lp  = lilv_plugins_get_by_uri(all, uri_node);
    lilv_node_free(uri_node);
    if (!lp) {
        lv2_log_error(&plugin->logger,
                      "modgui-host: hosted plugin not found: %s\n", uri);
        return false;
    }

    uint32_t n_ports = lilv_plugin_get_num_ports(lp);

    LilvNode* audio_class   = lilv_new_uri(plugin->world, LV2_CORE__AudioPort);
    LilvNode* control_class = lilv_new_uri(plugin->world, LV2_CORE__ControlPort);
    LilvNode* input_class   = lilv_new_uri(plugin->world, LV2_CORE__InputPort);
    LilvNode* output_class  = lilv_new_uri(plugin->world, LV2_CORE__OutputPort);
    LilvNode* lv2_min       = lilv_new_uri(plugin->world, LV2_CORE__minimum);
    LilvNode* lv2_max       = lilv_new_uri(plugin->world, LV2_CORE__maximum);
    LilvNode* lv2_default   = lilv_new_uri(plugin->world, LV2_CORE__default);

    int ctrl_count = 0;
    int audio_in_count = 0, audio_out_count = 0;

    /* Count control ports first */
    for (uint32_t i = 0; i < n_ports; i++) {
        const LilvPort* port = lilv_plugin_get_port_by_index(lp, i);
        if (lilv_port_is_a(lp, port, control_class))
            ctrl_count++;
    }

    out->ctrl_ports   = (ControlPort*)calloc((size_t)ctrl_count,
                                              sizeof(ControlPort));
    out->ctrl_buffers = (float*)calloc((size_t)ctrl_count, sizeof(float));

    /* Build port info */
    for (uint32_t i = 0; i < n_ports; i++) {
        const LilvPort* port = lilv_plugin_get_port_by_index(lp, i);
        bool is_audio   = lilv_port_is_a(lp, port, audio_class);
        bool is_control = lilv_port_is_a(lp, port, control_class);
        bool is_input   = lilv_port_is_a(lp, port, input_class);
        bool is_output  = lilv_port_is_a(lp, port, output_class);

        if (is_audio && is_input) {
            if (audio_in_count == 0) out->ai_l = (int)i;
            else if (audio_in_count == 1) out->ai_r = (int)i;
            audio_in_count++;
        } else if (is_audio && is_output) {
            if (audio_out_count == 0) out->ao_l = (int)i;
            else if (audio_out_count == 1) out->ao_r = (int)i;
            audio_out_count++;
        } else if (is_control) {
            int ci = out->n_ctrl_ports++;
            const LilvNode* sym_node = lilv_port_get_symbol(lp, port);
            out->ctrl_ports[ci].symbol     = strdup(lilv_node_as_string(sym_node));
            out->ctrl_ports[ci].port_index = i;
            out->ctrl_ports[ci].is_input   = is_input && !is_output;

            LilvScalePoints* sp = NULL; (void)sp;
            LilvNode* mn = lilv_port_get(lp, port, lv2_min);
            LilvNode* mx = lilv_port_get(lp, port, lv2_max);
            LilvNode* df = lilv_port_get(lp, port, lv2_default);

            out->ctrl_ports[ci].min = mn ? lilv_node_as_float(mn) : 0.0f;
            out->ctrl_ports[ci].max = mx ? lilv_node_as_float(mx) : 1.0f;
            out->ctrl_ports[ci].def = df ? lilv_node_as_float(df)
                                         : out->ctrl_ports[ci].min;
            out->ctrl_ports[ci].value  = out->ctrl_ports[ci].def;
            out->ctrl_buffers[ci]      = out->ctrl_ports[ci].def;

            lilv_node_free(mn);
            lilv_node_free(mx);
            lilv_node_free(df);
        }
    }

    lilv_node_free(audio_class);
    lilv_node_free(control_class);
    lilv_node_free(input_class);
    lilv_node_free(output_class);
    lilv_node_free(lv2_min);
    lilv_node_free(lv2_max);
    lilv_node_free(lv2_default);

    /* Store non-owning plugin reference for use in connect_hosted_ports */
    out->lp      = lp;
    out->n_ports = n_ports;

    /* Instantiate */
    out->instance = lilv_plugin_instantiate(lp, plugin->sample_rate, NULL);
    if (!out->instance) {
        lv2_log_error(&plugin->logger,
                      "modgui-host: failed to instantiate %s\n", uri);
        free_hosted_data(out);
        return false;
    }

    return true;
}

static void free_hosted_data(HostedData* d)
{
    if (!d) return;
    if (d->instance) {
        lilv_instance_deactivate(d->instance);
        lilv_instance_free(d->instance);
        d->instance = NULL;
    }
    for (int i = 0; i < d->n_ctrl_ports; i++)
        free(d->ctrl_ports[i].symbol);
    free(d->ctrl_ports);
    free(d->ctrl_buffers);
    free(d->uri);
    memset(d, 0, sizeof(*d));
    d->ai_l = d->ai_r = d->ao_l = d->ao_r = -1;
}

/*
 * Connect all control ports of the hosted plugin to their buffers.
 * Audio port connections are done in run() because the host may call
 * connect_port() at any time.
 */
static void connect_hosted_ports(ModguiHost* plugin, HostedData* d)
{
    if (!d->instance || !d->lp) return;
    const LilvPlugin* lp     = d->lp;
    uint32_t          n_ports = d->n_ports;

    LilvNode* control_class = lilv_new_uri(plugin->world, LV2_CORE__ControlPort);
    LilvNode* audio_class   = lilv_new_uri(plugin->world, LV2_CORE__AudioPort);

    /* First pass: connect all ports to dummy */
    for (uint32_t i = 0; i < n_ports; i++) {
        const LilvPort* port = lilv_plugin_get_port_by_index(lp, i);
        if (lilv_port_is_a(lp, port, audio_class)) {
            lilv_instance_connect_port(d->instance, i, plugin->dummy_audio);
        } else if (lilv_port_is_a(lp, port, control_class)) {
            lilv_instance_connect_port(d->instance, i, &plugin->dummy_float);
        } else {
            /* atom / MIDI / CV ports */
            lilv_instance_connect_port(d->instance, i, plugin->dummy_atom);
        }
    }

    /* Second pass: connect known control ports to their real buffers */
    for (int ci = 0; ci < d->n_ctrl_ports; ci++) {
        lilv_instance_connect_port(d->instance,
                                   d->ctrl_ports[ci].port_index,
                                   &d->ctrl_buffers[ci]);
    }

    lilv_node_free(control_class);
    lilv_node_free(audio_class);
}

static LV2_Worker_Status
work(LV2_Handle                  instance,
     LV2_Worker_Respond_Function  respond,
     LV2_Worker_Respond_Handle    respond_handle,
     uint32_t                     size,
     const void*                  data)
{
    (void)size;
    ModguiHost* plugin = (ModguiHost*)instance;
    const char* uri    = (const char*)data;

    HostedData* hd = (HostedData*)malloc(sizeof(HostedData));
    if (!hd) return LV2_WORKER_ERR_NO_SPACE;

    if (!load_hosted_plugin(plugin, uri, hd)) {
        free(hd);
        hd = NULL;
    } else {
        hd->uri = strdup(uri);
    }

    return respond(respond_handle, sizeof(HostedData*), &hd);
}

static LV2_Worker_Status
work_response(LV2_Handle instance, uint32_t size, const void* data)
{
    (void)size;
    ModguiHost* plugin  = (ModguiHost*)instance;
    HostedData* new_hd  = *(HostedData**)data;

    if (!new_hd) return LV2_WORKER_SUCCESS; /* load failed */

    /* Free old hosted plugin */
    HostedData old = plugin->hosted;
    plugin->hosted = *new_hd;
    free(new_hd);

    /* Connect all ports (non-audio) — done once here, not in run() */
    connect_hosted_ports(plugin, &plugin->hosted);

    /* Update stored URI (URI ownership transferred from HostedData) */
    free(plugin->hosted_plugin_uri);
    plugin->hosted_plugin_uri = plugin->hosted.uri;
    plugin->hosted.uri        = NULL;

    /* Activate */
    lilv_instance_activate(plugin->hosted.instance);

    /* Tell run() to push the URI to the UI on the next cycle */
    plugin->send_uri_to_ui = true;

    /* Free old data — safe here since we're in work_response (audio thread),
     * old instance is already detached */
    free_hosted_data(&old);

    return LV2_WORKER_SUCCESS;
}

static const LV2_Worker_Interface worker_interface = {
    .work          = work,
    .work_response = work_response,
    .end_run       = NULL,
};

/* ── State ───────────────────────────────────────────────────────────────── */

static LV2_State_Status
state_save(LV2_Handle                 instance,
           LV2_State_Store_Function   store,
           LV2_State_Handle           handle,
           uint32_t                   flags,
           const LV2_Feature* const*  features)
{
    (void)flags; (void)features;
    ModguiHost* plugin = (ModguiHost*)instance;
    if (!plugin->hosted_plugin_uri) return LV2_STATE_SUCCESS;

    LV2_URID_Map* map = plugin->map;
    LV2_URID key = map->map(map->handle, MODGUI_HOSTED_PLUGIN_URI);
    LV2_URID type = map->map(map->handle, LV2_ATOM__String);

    return store(handle, key,
                 plugin->hosted_plugin_uri,
                 strlen(plugin->hosted_plugin_uri) + 1,
                 type,
                 LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE);
}

static LV2_State_Status
state_restore(LV2_Handle                  instance,
              LV2_State_Retrieve_Function  retrieve,
              LV2_State_Handle             handle,
              uint32_t                     flags,
              const LV2_Feature* const*    features)
{
    (void)flags; (void)features;
    ModguiHost* plugin = (ModguiHost*)instance;
    LV2_URID_Map* map  = plugin->map;

    LV2_URID key  = map->map(map->handle, MODGUI_HOSTED_PLUGIN_URI);
    LV2_URID type = map->map(map->handle, LV2_ATOM__String);

    size_t   size  = 0;
    uint32_t vtype = 0;
    uint32_t vflags = 0;
    const void* val = retrieve(handle, key, &size, &vtype, &vflags);

    if (val && vtype == type && size > 1) {
        free(plugin->hosted_plugin_uri);
        plugin->hosted_plugin_uri = strdup((const char*)val);

        /* Schedule load */
        plugin->schedule->schedule_work(plugin->schedule->handle,
                                         (uint32_t)size, val);
    }
    return LV2_STATE_SUCCESS;
}

static const LV2_State_Interface state_interface = {
    .save    = state_save,
    .restore = state_restore,
};

/* ── Extension data ──────────────────────────────────────────────────────── */

static const void*
extension_data(const char* uri)
{
    if (!strcmp(uri, LV2_STATE__interface))  return &state_interface;
    if (!strcmp(uri, LV2_WORKER__interface)) return &worker_interface;
    return NULL;
}

/* ── Descriptor ──────────────────────────────────────────────────────────── */

static const LV2_Descriptor descriptor = {
    .URI            = MODGUI_HOST_PLUGIN_URI,
    .instantiate    = instantiate,
    .connect_port   = connect_port,
    .activate       = activate,
    .run            = run,
    .deactivate     = deactivate,
    .cleanup        = cleanup,
    .extension_data = extension_data,
};

LV2_SYMBOL_EXPORT const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
