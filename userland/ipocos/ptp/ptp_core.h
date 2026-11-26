/* SPDX-License-Identifier: MIT
 * The phone as a PTP still-image camera (ISO 15740 over USB, read only), as an iPhone shows itself to Photos, Image
 * Capture, Windows Explorer and gphoto2: DCIM's folders and pictures, listed, described and read. Pure: the daemon
 * (hakios_ptpd.c) fills the object table and moves the bytes; this answers the containers.
 *
 * USB container: length (4), type (2: 1 command, 2 data, 3 response), code (2), transaction (4), parameters (4 each),
 * little-endian. A data phase is one USB transfer (the transport ends it with a short or zero-length packet).
 */
#ifndef PTP_CORE_H
#define PTP_CORE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum {
	PTP_COMMAND = 1, PTP_DATA = 2, PTP_RESPONSE = 3,
	PTP_GET_DEVICE_INFO = 0x1001, PTP_OPEN_SESSION = 0x1002, PTP_CLOSE_SESSION = 0x1003,
	PTP_GET_STORAGE_IDS = 0x1004, PTP_GET_STORAGE_INFO = 0x1005, PTP_GET_NUM_OBJECTS = 0x1006,
	PTP_GET_OBJECT_HANDLES = 0x1007, PTP_GET_OBJECT_INFO = 0x1008, PTP_GET_OBJECT = 0x1009,
	PTP_GET_THUMB = 0x100a, PTP_GET_PARTIAL_OBJECT = 0x101b,
	PTP_OK = 0x2001, PTP_GENERAL_ERROR = 0x2002, PTP_SESSION_NOT_OPEN = 0x2003,
	PTP_OPERATION_NOT_SUPPORTED = 0x2005, PTP_INVALID_STORAGE_ID = 0x2008, PTP_INVALID_OBJECT_HANDLE = 0x2009,
	PTP_NO_THUMBNAIL = 0x2010, PTP_SPECIFICATION_BY_FORMAT_UNSUPPORTED = 0x2014, PTP_INVALID_PARENT = 0x201a,
	PTP_INVALID_PARAMETER = 0x201d, PTP_SESSION_ALREADY_OPEN = 0x201e,
	PTP_FORMAT_UNDEFINED = 0x3000, PTP_FORMAT_ASSOCIATION = 0x3001, PTP_FORMAT_MPEG = 0x300b,
	PTP_FORMAT_QUICKTIME = 0x300d, PTP_FORMAT_JPEG = 0x3801, PTP_FORMAT_GIF = 0x3807, PTP_FORMAT_PNG = 0x380b,
	PTP_FORMAT_TIFF = 0x380d,
	PTP_STORAGE = 0x00010001,
	PTP_MAX_OBJECTS = 8192,
};

struct ptp_object {
	uint32_t handle, parent;        /* parent 0: at the storage's root */
	uint16_t format;
	uint64_t size;
	char name[128];
	char date[20];                  /* YYYYMMDDThhmmss */
};

struct ptp_ops {
	/* fill the table (stable handles: the same file keeps its handle); returns how many */
	int (*scan)(void *ctx, struct ptp_object *objects, int max);
	/* bytes of an object's file from an offset; returns how many, or -1 */
	long (*read)(void *ctx, const struct ptp_object *object, uint64_t offset, uint8_t *buffer, size_t length);
	void (*out)(void *ctx, const uint8_t *bytes, size_t length);   /* to the host, into the current transfer */
	void (*end)(void *ctx);                                       /* the current transfer ends here */
};

struct ptp {
	const struct ptp_ops *ops;
	void *ctx;
	uint32_t session;
	int count;
	uint64_t capacity, free_bytes;
	const char *model, *version, *serial;
	struct ptp_object objects[PTP_MAX_OBJECTS];
	uint8_t data[64 * 1024];
	size_t data_length;
	size_t in_length;
	uint8_t in[1024];
	uint64_t commands, bytes_out;
};

static inline void ptp_wr16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void ptp_wr32(uint8_t *p, uint32_t v) { ptp_wr16(p, (uint16_t)v); ptp_wr16(p + 2, (uint16_t)(v >> 16)); }
static inline uint16_t ptp_rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t ptp_rd32(const uint8_t *p) { return ptp_rd16(p) | (uint32_t)ptp_rd16(p + 2) << 16; }

/* the dataset being built */
static inline void ptp_u8(struct ptp *p, uint8_t v) { if (p->data_length < sizeof(p->data)) p->data[p->data_length++] = v; }
static inline void ptp_u16(struct ptp *p, uint16_t v) { ptp_u8(p, (uint8_t)v); ptp_u8(p, (uint8_t)(v >> 8)); }
static inline void ptp_u32(struct ptp *p, uint32_t v) { ptp_u16(p, (uint16_t)v); ptp_u16(p, (uint16_t)(v >> 16)); }
static inline void ptp_u64(struct ptp *p, uint64_t v) { ptp_u32(p, (uint32_t)v); ptp_u32(p, (uint32_t)(v >> 32)); }
static inline void ptp_string(struct ptp *p, const char *s)
{
	size_t n = s ? strlen(s) : 0;
	if (n > 254)
		n = 254;
	if (!n) {
		ptp_u8(p, 0);
		return;
	}
	ptp_u8(p, (uint8_t)(n + 1));
	for (size_t i = 0; i < n; i++)
		ptp_u16(p, (uint8_t)s[i]);
	ptp_u16(p, 0);
}
static inline void ptp_array16(struct ptp *p, const uint16_t *v, uint32_t n)
{
	ptp_u32(p, n);
	for (uint32_t i = 0; i < n; i++)
		ptp_u16(p, v[i]);
}

static inline void ptp_init(struct ptp *p, const struct ptp_ops *ops, void *ctx)
{
	memset(p, 0, sizeof(*p));
	p->ops = ops;
	p->ctx = ctx;
	p->model = "iPhone";
	p->version = "";
	p->serial = "";
}

static inline void ptp_header(uint8_t *h, uint32_t length, uint16_t type, uint16_t code, uint32_t transaction)
{
	ptp_wr32(h, length);
	ptp_wr16(h + 4, type);
	ptp_wr16(h + 6, code);
	ptp_wr32(h + 8, transaction);
}

static inline void ptp_send_data(struct ptp *p, uint16_t code, uint32_t transaction)
{
	uint8_t h[12];
	ptp_header(h, (uint32_t)(12 + p->data_length), PTP_DATA, code, transaction);
	p->ops->out(p->ctx, h, 12);
	if (p->data_length)
		p->ops->out(p->ctx, p->data, p->data_length);
	p->ops->end(p->ctx);
	p->bytes_out += 12 + p->data_length;
}

static inline void ptp_respond(struct ptp *p, uint16_t code, uint32_t transaction, const uint32_t *params, int n)
{
	uint8_t r[12 + 20];
	ptp_header(r, (uint32_t)(12 + 4 * n), PTP_RESPONSE, code, transaction);
	for (int i = 0; i < n; i++)
		ptp_wr32(r + 12 + 4 * i, params[i]);
	p->ops->out(p->ctx, r, (size_t)(12 + 4 * n));
	p->ops->end(p->ctx);
}

static inline struct ptp_object *ptp_find(struct ptp *p, uint32_t handle)
{
	for (int i = 0; i < p->count; i++)
		if (p->objects[i].handle == handle)
			return &p->objects[i];
	return NULL;
}

/* the objects GetObjectHandles / GetNumObjects mean: parent 0xffffffff the root's, 0 all, else that folder's */
static inline int ptp_selected(struct ptp *p, uint32_t format, uint32_t parent, uint32_t *out, int max)
{
	int n = 0;
	for (int i = 0; i < p->count; i++) {
		const struct ptp_object *o = &p->objects[i];
		if (format && o->format != format)
			continue;
		if (parent == 0xffffffffu ? o->parent != 0 : (parent && o->parent != parent))
			continue;
		if (n < max && out)
			out[n] = o->handle;
		n++;
	}
	return n;
}

static inline void ptp_command(struct ptp *p, uint16_t code, uint32_t transaction, const uint32_t *param, int nparams)
{
	static const uint16_t operations[] = {PTP_GET_DEVICE_INFO, PTP_OPEN_SESSION, PTP_CLOSE_SESSION,
	    PTP_GET_STORAGE_IDS, PTP_GET_STORAGE_INFO, PTP_GET_NUM_OBJECTS, PTP_GET_OBJECT_HANDLES, PTP_GET_OBJECT_INFO,
	    PTP_GET_OBJECT, PTP_GET_THUMB, PTP_GET_PARTIAL_OBJECT};
	static const uint16_t formats[] = {PTP_FORMAT_ASSOCIATION, PTP_FORMAT_JPEG, PTP_FORMAT_PNG, PTP_FORMAT_GIF,
	    PTP_FORMAT_TIFF, PTP_FORMAT_QUICKTIME, PTP_FORMAT_MPEG, PTP_FORMAT_UNDEFINED};
	uint32_t a = nparams > 0 ? param[0] : 0, b = nparams > 1 ? param[1] : 0, c = nparams > 2 ? param[2] : 0;
	p->commands++;
	p->data_length = 0;
	if (code == PTP_GET_DEVICE_INFO) {
		ptp_u16(p, 100);                /* PTP 1.00 */
		ptp_u32(p, 0);                  /* no vendor extension */
		ptp_u16(p, 0);
		ptp_string(p, "");
		ptp_u16(p, 0);                  /* standard mode */
		ptp_array16(p, operations, sizeof(operations) / sizeof(operations[0]));
		ptp_array16(p, NULL, 0);        /* events */
		ptp_array16(p, NULL, 0);        /* device properties */
		ptp_array16(p, NULL, 0);        /* capture formats */
		ptp_array16(p, formats, sizeof(formats) / sizeof(formats[0]));
		ptp_string(p, "Apple Inc.");
		ptp_string(p, p->model);
		ptp_string(p, p->version);
		ptp_string(p, p->serial);
		ptp_send_data(p, code, transaction);
		ptp_respond(p, PTP_OK, transaction, NULL, 0);
		return;
	}
	if (code == PTP_OPEN_SESSION) {
		if (!a) {
			ptp_respond(p, PTP_INVALID_PARAMETER, transaction, NULL, 0);
		} else {
			/* a session still open is one the computer lost (unplugged, reset): the new one replaces it */
			p->session = a;
			p->count = p->ops->scan(p->ctx, p->objects, PTP_MAX_OBJECTS);
			ptp_respond(p, PTP_OK, transaction, NULL, 0);
		}
		return;
	}
	if (!p->session) {
		ptp_respond(p, PTP_SESSION_NOT_OPEN, transaction, NULL, 0);
		return;
	}
	switch (code) {
	case PTP_CLOSE_SESSION:
		p->session = 0;
		ptp_respond(p, PTP_OK, transaction, NULL, 0);
		return;
	case PTP_GET_STORAGE_IDS:
		ptp_u32(p, 1);
		ptp_u32(p, PTP_STORAGE);
		ptp_send_data(p, code, transaction);
		ptp_respond(p, PTP_OK, transaction, NULL, 0);
		return;
	case PTP_GET_STORAGE_INFO:
		if (a != PTP_STORAGE) {
			ptp_respond(p, PTP_INVALID_STORAGE_ID, transaction, NULL, 0);
			return;
		}
		ptp_u16(p, 0x0003);             /* fixed RAM */
		ptp_u16(p, 0x0002);             /* generic hierarchical */
		ptp_u16(p, 0x0001);             /* read only, no deletion */
		ptp_u64(p, p->capacity);
		ptp_u64(p, p->free_bytes);
		ptp_u32(p, 0xffffffffu);
		ptp_string(p, p->model);
		ptp_string(p, "");
		ptp_send_data(p, code, transaction);
		ptp_respond(p, PTP_OK, transaction, NULL, 0);
		return;
	case PTP_GET_NUM_OBJECTS:
	case PTP_GET_OBJECT_HANDLES: {
		if (a != PTP_STORAGE && a != 0xffffffffu) {
			ptp_respond(p, PTP_INVALID_STORAGE_ID, transaction, NULL, 0);
			return;
		}
		if (c && c != 0xffffffffu && !ptp_find(p, c)) {
			ptp_respond(p, PTP_INVALID_PARENT, transaction, NULL, 0);
			return;
		}
		p->count = p->ops->scan(p->ctx, p->objects, PTP_MAX_OBJECTS);   /* what is there now */
		static uint32_t handles[PTP_MAX_OBJECTS];
		int n = ptp_selected(p, b, c, handles, PTP_MAX_OBJECTS);
		if (code == PTP_GET_NUM_OBJECTS) {
			uint32_t count = (uint32_t)n;
			ptp_respond(p, PTP_OK, transaction, &count, 1);
			return;
		}
		/* up to 16k handles fit the 64 KiB dataset */
		ptp_u32(p, (uint32_t)n);
		for (int i = 0; i < n; i++)
			ptp_u32(p, handles[i]);
		ptp_send_data(p, code, transaction);
		ptp_respond(p, PTP_OK, transaction, NULL, 0);
		return;
	}
	case PTP_GET_OBJECT_INFO: {
		const struct ptp_object *o = ptp_find(p, a);
		if (!o) {
			ptp_respond(p, PTP_INVALID_OBJECT_HANDLE, transaction, NULL, 0);
			return;
		}
		ptp_u32(p, PTP_STORAGE);
		ptp_u16(p, o->format);
		ptp_u16(p, 0);                  /* no protection */
		ptp_u32(p, o->size > 0xffffffffu ? 0xffffffffu : (uint32_t)o->size);
		ptp_u16(p, 0);                  /* no thumbnail */
		ptp_u32(p, 0);
		ptp_u32(p, 0);
		ptp_u32(p, 0);
		ptp_u32(p, 0);                  /* image width, height, depth: unknown */
		ptp_u32(p, 0);
		ptp_u32(p, 0);
		ptp_u32(p, o->parent);
		ptp_u16(p, o->format == PTP_FORMAT_ASSOCIATION ? 0x0001 : 0);    /* generic folder */
		ptp_u32(p, 0);
		ptp_u32(p, 0);
		ptp_string(p, o->name);
		ptp_string(p, o->date);
		ptp_string(p, o->date);
		ptp_string(p, "");
		ptp_send_data(p, code, transaction);
		ptp_respond(p, PTP_OK, transaction, NULL, 0);
		return;
	}
	case PTP_GET_OBJECT:
	case PTP_GET_PARTIAL_OBJECT: {
		const struct ptp_object *o = ptp_find(p, a);
		if (!o || o->format == PTP_FORMAT_ASSOCIATION) {
			ptp_respond(p, PTP_INVALID_OBJECT_HANDLE, transaction, NULL, 0);
			return;
		}
		uint64_t offset = code == PTP_GET_PARTIAL_OBJECT ? b : 0;
		uint64_t length = o->size > offset ? o->size - offset : 0;
		if (code == PTP_GET_PARTIAL_OBJECT && length > c)
			length = c;
		uint8_t h[12];
		ptp_header(h, 12 + length > 0xffffffffu ? 0xffffffffu : (uint32_t)(12 + length), PTP_DATA, code, transaction);
		p->ops->out(p->ctx, h, 12);
		uint64_t done = 0;
		while (done < length) {
			size_t want = length - done > sizeof(p->data) ? sizeof(p->data) : (size_t)(length - done);
			long got = p->ops->read(p->ctx, o, offset + done, p->data, want);
			if (got <= 0) {
				/* the file changed under us: the rest as zeros, so the transfer has the length it announced */
				memset(p->data, 0, want);
				got = (long)want;
			}
			p->ops->out(p->ctx, p->data, (size_t)got);
			done += (uint64_t)got;
		}
		p->ops->end(p->ctx);
		p->bytes_out += 12 + length;
		if (code == PTP_GET_PARTIAL_OBJECT) {
			uint32_t sent = (uint32_t)length;
			ptp_respond(p, PTP_OK, transaction, &sent, 1);
		} else {
			ptp_respond(p, PTP_OK, transaction, NULL, 0);
		}
		return;
	}
	case PTP_GET_THUMB:
		ptp_respond(p, ptp_find(p, a) ? PTP_NO_THUMBNAIL : PTP_INVALID_OBJECT_HANDLE, transaction, NULL, 0);
		return;
	default:
		ptp_respond(p, PTP_OPERATION_NOT_SUPPORTED, transaction, NULL, 0);
		return;
	}
}

/* bytes from the host's bulk OUT endpoint, in any slicing; command containers answered, data phases from the host
 * (none of the operations here take one) skipped */
static inline void ptp_input(struct ptp *p, const uint8_t *bytes, size_t length)
{
	while (length) {
		size_t n = sizeof(p->in) - p->in_length;
		if (n > length)
			n = length;
		memcpy(p->in + p->in_length, bytes, n);
		p->in_length += n;
		bytes += n;
		length -= n;
		while (p->in_length >= 12) {
			uint32_t total = ptp_rd32(p->in);
			uint16_t type = ptp_rd16(p->in + 4);
			if (total < 12 || (type == PTP_COMMAND && total > 32)) {
				p->in_length = 0;       /* lost framing: start again */
				break;
			}
			if (type != PTP_COMMAND) {
				/* a data phase from the host: drop its bytes */
				size_t drop = total <= p->in_length ? total : p->in_length;
				memmove(p->in, p->in + drop, p->in_length - drop);
				p->in_length -= drop;
				if (drop < total)
					break;              /* (a long one spans reads: the rest is lost framing, reset above) */
				continue;
			}
			if (p->in_length < total)
				break;
			uint32_t params[5] = {0};
			int nparams = (int)(total - 12) / 4;
			for (int i = 0; i < nparams; i++)
				params[i] = ptp_rd32(p->in + 12 + 4 * i);
			uint16_t code = ptp_rd16(p->in + 6);
			uint32_t transaction = ptp_rd32(p->in + 8);
			memmove(p->in, p->in + total, p->in_length - total);
			p->in_length -= total;
			ptp_command(p, code, transaction, params, nparams);
		}
	}
}
#endif
