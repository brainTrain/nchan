#include <ngx_http_push_module.h>
#include "../store/memory/shmem.h"
#include "../store/memory/ipc.h"
#include "../store/memory/store-private.h"
#include "../store/memory/ipc-handlers.h"
#include "internal.h"

#define MEMSTORE_IPC_SUBSCRIBER_TIMEOUT 5

#define DEBUG_LEVEL NGX_LOG_WARN
#define DBG(fmt, arg...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "SUB:MEM-IPC:" fmt, ##arg)
#define ERR(fmt, arg...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "SUB:MEM-IPC:" fmt, ##arg)


typedef struct sub_data_s sub_data_t;

typedef struct {
  sub_data_t     *d;
  subscriber_t   *sub;
} timeout_data_t;

struct sub_data_s {
  ngx_str_t      *chid;
  ngx_int_t       originator;
  void           *foreign_chanhead;
  ngx_event_t     timeout_ev;
  timeout_data_t *timeout_data;
}; //sub_data_t

static ngx_int_t empty_callback(){
  return NGX_OK;
}

static ngx_int_t sub_enqueue(ngx_int_t timeout, void *ptr, sub_data_t *d) {
  DBG("memstore subsriber enqueued ok");
  return NGX_OK;
}

static ngx_int_t sub_dequeue(ngx_int_t status, void *ptr, sub_data_t* d) {
  DBG("memstore subscriber dequeue: notify owner");
  if(d->timeout_ev.timer_set) {
    ngx_del_timer(&d->timeout_ev);
  }
  return memstore_ipc_send_unsubscribed(d->originator, d->chid, NULL);
}

static ngx_int_t sub_respond_message(ngx_int_t status, void *ptr, sub_data_t* d) {
  DBG("memstore subscriber respond with message");
  ngx_http_push_msg_t     *msg = (ngx_http_push_msg_t *) ptr;
  return memstore_ipc_send_publish_message(d->originator, d->chid, msg, 50, 0, 0, empty_callback, NULL);
}

static ngx_int_t sub_respond_status(ngx_int_t status, void *ptr, sub_data_t *d) {
  DBG("memstore subscriber respond with status");
  switch(status) {
    case NGX_HTTP_NO_CONTENT: //message expired
    case NGX_HTTP_GONE: //delete
    case NGX_HTTP_CLOSE: //delete
    case NGX_HTTP_NOT_MODIFIED: //timeout?
    case NGX_HTTP_FORBIDDEN:
      //do nothing, will be dequeued automatically
      break;
    default:
      ERR("unknown status %i", status);
  }
  return NGX_OK;
}
static void reset_timer(sub_data_t *data) {
  DBG("RESET tIMER");
  if(data->timeout_ev.timer_set) {
    ngx_del_timer(&data->timeout_ev);
  }
  ngx_add_timer(&data->timeout_ev, MEMSTORE_IPC_SUBSCRIBER_TIMEOUT * 1000);
}

static void timeout_ev_handler(ngx_event_t *ev) {
  timeout_data_t *d = (timeout_data_t *)ev->data;
  DBG("timeout event. dequeue!");
  d->sub->dequeue(d->sub);
}
typedef struct {
  sub_data_t     d;
  timeout_data_t t;
} memstore_subscriber_slab_t;

subscriber_t *memstore_subscriber_create(ngx_int_t originator_slot, ngx_str_t *chid, void* foreign_chanhead) {
  DBG("memstore subscriver create with privdata %p");
  sub_data_t                 *d;
  memstore_subscriber_slab_t *s;
  s = ngx_alloc(sizeof(*s), ngx_cycle->log);
  if(s == NULL) {
    ERR("couldn't allocate memstore subscriber data");
    return NULL;
  }
  d = &s->d;
  d->timeout_data = &s->t;
  
  subscriber_t *sub = internal_subscriber_create(d);
  internal_subscriber_set_name(sub, "memstore-ipc");
  internal_subscriber_set_enqueue_handler(sub, (callback_pt )sub_enqueue);
  internal_subscriber_set_dequeue_handler(sub, (callback_pt )sub_dequeue);
  internal_subscriber_set_respond_message_handler(sub, (callback_pt )sub_respond_message);
  internal_subscriber_set_respond_status_handler(sub, (callback_pt )sub_respond_status);
  d->chid = chid;
  d->originator = originator_slot;
  d->foreign_chanhead = foreign_chanhead;
  
  ngx_memzero(&d->timeout_ev, sizeof(d->timeout_ev));
  d->timeout_ev.handler = timeout_ev_handler;
  d->timeout_ev.data = &s->t;
  s->t.sub = sub;
  s->t.d=d;
  d->timeout_ev.log = ngx_cycle->log;
  reset_timer(d);
  return sub;
}
