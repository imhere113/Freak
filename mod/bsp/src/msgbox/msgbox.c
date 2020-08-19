#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "box.h"
#include "log.h"
#include "inc/msgbox.h"

static box_t s_box = NULL;

int msgbox_init()
{
    s_box = box_new(PROTP_BSP_KEY_MAX);
    return !s_box;
}

int msgbox_deinit()
{
    box_free(s_box);
    return 0;
}

int msgbox_add_handler(uint32_t key, msgbox_handler_t func)
{
    return box_add(s_box, key, (void *)func);
}

int msgbox_do_handler(void *in, size_t isize, void *out, size_t *osize)
{
    size_t size = 0;
    proto_header_t *packet_in = in;
    proto_header_t *packet_out = out;
    msgbox_handler_t func = box_get(s_box, packet_in->key, NULL);
    assert(func);

    proto_header_dump(packet_in);
    memcpy(packet_out, packet_in, sizeof(proto_header_t));
    packet_out->errcode = func(packet_in->chn, packet_in->action,
                               packet_in->data, packet_in->size,
                               packet_out->data, &size);
    packet_out->size = size;
    proto_header_dump(packet_out);

    *osize = packet_out->size + sizeof(proto_header_t);
    return 0;
}

