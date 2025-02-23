#include <icd/icd_gconf.h>

#include "ofono-private.h"
#include "ofono-modem.h"

#include "log.h"
#include "link.h"
#include "icd-gconf.h"

static struct modem_data *
ofono_modem_find_by_imsi(ofono_private *priv, const gchar *imsi)
{
  GHashTableIter iter;
  gpointer key, value;

  g_hash_table_iter_init (&iter, priv->modems);

  while (g_hash_table_iter_next (&iter, &key, &value))
  {
    struct modem_data *md = value;

    if (ofono_simmgr_valid(md->sim) && md->sim->present &&
        !g_strcmp0(md->sim->imsi, imsi))
    {
      return md;
    }
  }

  return NULL;
}

struct connctx_data
{
  ofono_private *priv;
  gchar *network_id;
  gchar *network_type;
  guint network_attrs;
  icd_nw_link_up_cb_fn link_up_cb;
  gpointer link_up_cb_token;
  struct modem_data *md;
  OfonoConnCtx *ctx;
  guint timeout_id;
  gulong id;
  gboolean connected;
};

static gboolean
_link_up_idle(gpointer user_data)
{
  struct connctx_data *data = user_data;
  ofono_private *priv = data->priv;
  gchar *net_id = data->network_id;
  OfonoConnCtx *ctx = data->ctx;
  const OfonoConnCtxSettings* s = ctx->settings;
  gchar *ipv4_type = ofono_icd_gconf_get_iap_string(priv, net_id, "ipv4_type");
  gboolean ipv4_autodns = ofono_icd_gconf_get_iap_bool(priv, net_id,
                                                       "ipv4_autodns", TRUE);

  if (!ipv4_type)
  {
    ipv4_type = g_strdup("AUTO");
    ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_type", "AUTO");
  }

  OFONO_DEBUG("Calling next layer, ipv4_type: %s", ipv4_type);
  OFONO_DEBUG("ipv4 settings: %s %s (gw %s) (nm %s) (dns %s %s)",
              s->ifname, s->address, s->gateway, s->netmask,
              s->dns[0], s->dns[0] ? s->dns[1] : s->dns[0]);

  /* hack settings so next layer to take it from there */
  if (!strcmp(ipv4_type, "AUTO"))
  {
    if (s->method != OFONO_CONNCTX_METHOD_DHCP)
    {
      ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_address", s->address);
      ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_gateway", s->gateway);
      ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_netmask", s->netmask);
      ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_type", "STATIC");
    }
    else
      OFONO_DEBUG("ipv4 settings: dhcp");
  }

  if (ipv4_autodns)
  {
    OFONO_DEBUG("Using ofono provided DNS addresses");
    ofono_icd_gconf_set_iap_bool(priv, net_id, "ipv4_autodns", FALSE);

    if (s->dns[0])
    {
      ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_dns1", s->dns[0]);

      if (s->dns[1])
        ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_dns2", s->dns[1]);
      else
        ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_dns2", NULL);
    }
    else
    {
      ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_dns1", NULL);
      ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_dns2", NULL);
    }
  }
  else
    OFONO_DEBUG("Using manual DNS addresses");

  data->timeout_id = 0;
  data->link_up_cb(ICD_NW_SUCCESS_NEXT_LAYER, NULL, data->ctx->settings->ifname,
                   data->link_up_cb_token, NULL);
  data->connected = TRUE;

  /* restore what we found initially */
  ofono_icd_gconf_set_iap_bool(priv, net_id, "ipv4_autodns", ipv4_autodns);
  ofono_icd_gconf_set_iap_string(priv, net_id, "ipv4_type", ipv4_type);
  g_free(ipv4_type);

  return G_SOURCE_REMOVE;
}

static void
ofono_connctx_set_string_complete(OfonoConnCtx *ctx, const GError *error,
                                  void *arg)
{
    GList **l = arg;

    if (error)
    {
        OFONO_WARN("Unable to set context property '%s': %s",
                   (gchar *)(*l)->data,
                   error->message ? error->message : "Unknown error");
    }
    else
      OFONO_DEBUG("Context property '%s' set", (gchar *)(*l)->data);


    g_free((*l)->data);
    *l = g_list_delete_link(*l, *l);

    if (*l == NULL)
    {
      g_free(l);
      ofono_connctx_activate(ctx);
    }
}


static void
connctx_activate(struct connctx_data *data, gboolean activate)
{
  if (activate)
  {
    ofono_private *priv = data->priv;
    gchar *iap_name = data->network_id;
    OfonoConnCtx *ctx = data->ctx;
    gchar *s;
    GList **l = g_new0(GList *, 1);

    OFONO_DEBUG("Activate ctx: %p", data->ctx);

    s = ofono_icd_gconf_get_iap_string(priv, iap_name, "gprs_accesspointname");

    if (g_strcmp0(ctx->apn, s))
    {
      *l = g_list_prepend(*l, g_strdup("AccessPointName"));
      ofono_connctx_set_string_full(ctx, "AccessPointName", s,
                                    ofono_connctx_set_string_complete, l);
    }

    g_free(s);

    s = ofono_icd_gconf_get_iap_string(priv, iap_name, "gprs_username");

    if (g_strcmp0(ctx->username, s))
    {
      *l = g_list_prepend(*l, g_strdup("Username"));
      ofono_connctx_set_string_full(ctx, "Username", s,
                                    ofono_connctx_set_string_complete, l);
    }

    g_free(s);

    s = ofono_icd_gconf_get_iap_string(priv, iap_name, "gprs_password");

    if (g_strcmp0(ctx->password, s))
    {
      *l = g_list_prepend(*l, g_strdup("Password"));
      ofono_connctx_set_string_full(ctx, "Password", s,
                                    ofono_connctx_set_string_complete, l);
    }

    g_free(s);

    if (*l == NULL)
    {
      g_free(l);
      ofono_connctx_activate(data->ctx);
    }
  }
  else
  {
    OFONO_DEBUG("Dectivate ctx: %p", data->ctx);
    ofono_connctx_deactivate(data->ctx);
  }
}

static void
_ctx_active_changed_cb(OfonoConnCtx *ctx, void* user_data)
{
  struct connctx_data *data = user_data;

  OFONO_DEBUG("ctx %p active state changed to %d", ctx, ctx->active);

  if (!data->connected)
  {
    if (ctx->active)
      data->timeout_id = g_idle_add(_link_up_idle, data);
    else
      connctx_activate(data, TRUE);
  }
  else if (!ctx->active)
  {
    data->priv->close_fn(ICD_NW_ERROR, "network_error", data->network_type,
                         data->network_attrs, data->network_id);
    g_hash_table_remove(data->md->ctxhd, ctx);
  }
}

static void
_connctx_weak_notify(gpointer user_data, GObject *ctx)
{
  struct connctx_data *data = user_data;

  OFONO_DEBUG("ctx %p is being destroyed", ctx);

  if (data->connected)
  {
    data->priv->close_fn(ICD_NW_ERROR, "network_error", data->network_type,
                         data->network_attrs, data->network_id);

  }

  g_hash_table_remove(data->md->ctxhd, ctx);
}

void
ofono_connctx_handler_data_destroy(gpointer ctxd)
{
  struct connctx_data *data = ctxd;

  g_object_weak_unref((GObject *)data->ctx, _connctx_weak_notify, data);
  ofono_connctx_remove_handler(data->ctx, data->id);

  if (data->timeout_id)
    g_source_remove(data->timeout_id);

  g_free(data->network_id);
  g_free(data->network_type);
  g_free(data);
}

void
ofono_link_up(const gchar *network_type, const guint network_attrs,
              const gchar *network_id, icd_nw_link_up_cb_fn link_up_cb,
              const gpointer link_up_cb_token, gpointer *_priv)
{
  ofono_private *priv = *_priv;
  const char *err_msg = "no_network";
  gchar *imsi;
  gboolean error = TRUE;
  struct modem_data *md;
  gchar *context_id = NULL;
  OfonoConnCtx *ctx = NULL;
  struct connctx_data *data;

  OFONO_ENTER

  imsi = icd_gconf_get_iap_string(network_id, SIM_IMSI);

  if (!imsi)
  {
      OFONO_WARN("network_id %s is missing imsi gconf data", network_id);
      goto err;
  }

  OFONO_DEBUG("Got IMSI: %s", imsi);

  md = ofono_modem_find_by_imsi(priv, imsi);

  if (!md)
  {
    OFONO_WARN("No modem found for imsi %s", imsi);
    goto err;
  }

  OFONO_DEBUG("Got modem data");

  context_id = icd_gconf_get_iap_string(network_id, "context_id");

  if (!context_id || !*context_id)
  {
    OFONO_WARN("No context id found for iap %s", network_id);
    goto err;
  }

  OFONO_DEBUG("Got context id: %s", context_id);

  ctx = ofono_modem_get_context_by_id(md, context_id);

  if (!ctx)
  {
    OFONO_WARN("No context found for id %s, unprovision iap %s", context_id,
               network_id);
    ofono_icd_gconf_set_iap_string(priv, network_id, "context_id", NULL);
    goto err;
  }
  else
    OFONO_DEBUG("Got ctx: %p", ctx);

  data = g_try_new(struct connctx_data, 1);

  if (!data)
    goto err;

  data->priv = priv;
  data->network_type = g_strdup(network_type);
  data->network_attrs = network_attrs;
  data->network_id = g_strdup(network_id);
  data->link_up_cb = link_up_cb;
  data->link_up_cb_token = link_up_cb_token;
  data->md = md;
  data->ctx = ctx;
  data->timeout_id = 0;
  data->connected = FALSE;
  data->id = ofono_connctx_add_active_changed_handler(
        ctx, _ctx_active_changed_cb, data);

  g_hash_table_insert(md->ctxhd, ctx, data);

  /* in case ctx gets destroyed behind our back */
  g_object_weak_ref((GObject *)ctx, _connctx_weak_notify, data);

  connctx_activate(data, !ctx->active);
  error = FALSE;

err:
  g_free(context_id);
  g_free(imsi);

  if (error)
    link_up_cb(ICD_NW_ERROR, err_msg, NULL, link_up_cb_token, NULL);

  OFONO_EXIT
}

void
ofono_link_down(const gchar *network_type, const guint network_attrs,
                const gchar *network_id, const gchar *interface_name,
                icd_nw_link_down_cb_fn link_down_cb,
                const gpointer link_down_cb_token, gpointer *_priv)
{
  ofono_private *priv = *_priv;
  gchar *imsi;

  OFONO_ENTER

  OFONO_DEBUG("Getting IMSI");
  imsi = icd_gconf_get_iap_string(network_id, SIM_IMSI);

  if (imsi)
  {
    OFONO_DEBUG("Got IMSI: %s", imsi);
    struct modem_data *md = ofono_modem_find_by_imsi(priv, imsi);

    g_free(imsi);

    if (md)
    {
      OfonoConnCtx *ctx;
      gchar *context_id = icd_gconf_get_iap_string(network_id, "context_id");

      OFONO_DEBUG("Got modem data, id %s", context_id);

      ctx = ofono_modem_get_context_by_id(md, context_id);
      g_free(context_id);

      if (ctx)
      {
        g_hash_table_remove(md->ctxhd, ctx);

        if (ctx->active)
          ofono_connctx_deactivate(ctx);
      }

    }
  }

  link_down_cb(ICD_NW_SUCCESS, link_down_cb_token);

  OFONO_EXIT
}
