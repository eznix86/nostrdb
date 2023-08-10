
#include "nostrdb.h"
#include "jsmn.h"
#include "hex.h"
#include "cursor.h"
#include "random.h"
#include "sha256.h"
#include "lmdb.h"
#include "util.h"
#include "threadpool.h"
#include "protected_queue.h"
#include <stdlib.h>
#include <limits.h>
#include <assert.h>

#include "secp256k1.h"
#include "secp256k1_ecdh.h"
#include "secp256k1_schnorrsig.h"

// the maximum number of things threads pop and push in bulk
static const int THREAD_QUEUE_BATCH = 1024;

// the maximum size of inbox queues
static const int DEFAULT_QUEUE_SIZE = 50000;

enum ndb_dbs {
	NDB_DBI_ID,
};
#define NDB_DBIS 1

struct ndb_json_parser {
	const char *json;
	int json_len;
	struct ndb_builder builder;
	jsmn_parser json_parser;
	jsmntok_t *toks, *toks_end;
	int i;
	int num_tokens;
};

struct ndb_writer {
	void *queue_buf;
	int queue_buflen;
	pthread_t thread_id;

	struct prot_queue inbox;
};

struct ndb_ingester {
	struct threadpool tp;
	struct ndb_writer *writer;
};

struct ndb {
	MDB_env *env;
	struct ndb_ingester ingester;
	struct ndb_writer writer;
	// lmdb environ handles, etc
};

// A clustered key with an id and a timestamp
struct ndb_id_ts {
	unsigned char id[32];
	uint32_t created;
};

enum ndb_ingester_msgtype {
	NDB_INGEST_EVENT, // write json to the ingester queue for processing 
	NDB_INGEST_QUIT,  // kill ingester thread immediately
};

enum ndb_writer_msgtype {
	NDB_WRITER_NOTE, // write a note to the db
	NDB_WRITER_QUIT, // kill thread immediately
};

struct ndb_ingester_event {
	char *json;
	int len;
};

struct ndb_writer_note {
	struct ndb_note *note;
	size_t note_len;
};

struct ndb_ingester_msg {
	enum ndb_ingester_msgtype type;
	union {
		struct ndb_ingester_event event;
	};
};

struct ndb_writer_msg {
	enum ndb_writer_msgtype type;
	union {
		struct ndb_writer_note note;
	};
};

int ndb_note_verify(void *ctx, unsigned char pubkey[32], unsigned char id[32],
		    unsigned char sig[64])
{
	secp256k1_xonly_pubkey xonly_pubkey;
	int ok;

	ok = secp256k1_xonly_pubkey_parse((secp256k1_context*)ctx, &xonly_pubkey,
					  pubkey) != 0;
	if (!ok) return 0;

	ok = secp256k1_schnorrsig_verify((secp256k1_context*)ctx, sig, id, 32,
					 &xonly_pubkey) > 0;
	if (!ok) return 0;

	return 1;
}

static inline int ndb_writer_queue_msgs(struct ndb_writer *writer,
					struct ndb_writer_msg *msgs,
					int num_msgs)
{
	return prot_queue_push_all(&writer->inbox, msgs, num_msgs);
}

static int ndb_writer_queue_note(struct ndb_writer *writer,
				 struct ndb_note *note, size_t note_len)
{
	struct ndb_writer_msg msg;
	msg.type = NDB_WRITER_NOTE;

	msg.note.note = note;
	msg.note.note_len = note_len;

	return prot_queue_push(&writer->inbox, &msg);
}


static int ndb_ingester_process_event(secp256k1_context *ctx,
				      struct ndb_ingester *ingester,
				      struct ndb_ingester_event *ev,
				      struct ndb_writer_msg *out)
{
	struct ndb_tce tce;
	struct ndb_note *note;
	void *buf;
	size_t bufsize, note_size;

	// since we're going to be passing this allocated note to a different
	// thread, we can't use thread-local buffers. just allocate a block
        bufsize = max(ev->len * 8.0, 4096);
	buf = malloc(bufsize);
	if (!buf)
		return 0;

	note_size =
		ndb_ws_event_from_json(ev->json, ev->len, &tce, buf, bufsize);

	switch (tce.evtype) {
	case NDB_TCE_NOTICE: goto cleanup;
	case NDB_TCE_EOSE:   goto cleanup;
	case NDB_TCE_OK:     goto cleanup;
	case NDB_TCE_EVENT:
		note = tce.event.note;

		// Verify! If it's an invalid note we don't need to bothter
		// writing it to the database
		if (!ndb_note_verify(ctx, note->pubkey, note->id, note->sig)) {
			ndb_debug("signature verification failed\n");
			goto cleanup;
		}

		if (note != buf) {
			ndb_debug("note buffer not equal to malloc'd buffer\n");
			goto cleanup;
		}

		note = realloc(note, note_size);

		out->type = NDB_WRITER_NOTE;
		out->note.note = note;
		out->note.note_len = note_size;

		// there's nothing left to do with the original json, so free it
		free(ev->json);
		return 1;
	}

cleanup:
	free(ev->json);
	free(buf);

	return 0;
}

static void *ndb_writer_thread(void *data)
{
	struct ndb_writer *writer = data;
	struct ndb_writer_msg msgs[THREAD_QUEUE_BATCH], *msg;
	int i, popped;

	while (true) {
		popped = prot_queue_pop_all(&writer->inbox, msgs, THREAD_QUEUE_BATCH);
		ndb_debug("popped %d items in the writer thread\n", popped);

		// look for a quit message to quit ASAP
		for (i = 0; i < popped; i++) {
			msg = &msgs[i];
			if (msg->type == NDB_WRITER_QUIT)
				goto cleanup;
		}

		switch (msg->type) {
		case NDB_WRITER_QUIT:
			// quits are handled before this
			ndb_debug("writer: unexpected quit message\n");
			goto cleanup;
		case NDB_WRITER_NOTE:
			//ndb_debug("writing note %ld bytes\n", msg->note.note_len);
			free(msg->note.note);
		}
	}

cleanup:
	ndb_debug("quitting writer thread\n");
	return NULL;
}

static void *ndb_ingester_thread(void *data)
{
	secp256k1_context *ctx;
	struct thread *thread = data;
	struct ndb_ingester *ingester = (struct ndb_ingester *)thread->ctx;
	struct ndb_ingester_msg msgs[THREAD_QUEUE_BATCH], *msg;
	struct ndb_writer_msg outs[THREAD_QUEUE_BATCH], *out;
	int i, to_write, popped;

	ctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);
	ndb_debug("started ingester thread\n");

	while (true) {
		to_write = 0;
		popped = prot_queue_pop_all(&thread->inbox, msgs, THREAD_QUEUE_BATCH);
		ndb_debug("popped %d items in the ingester thread\n", popped);

		// look for a quit message to quit ASAP
		// TODO: should I drain the queue first ?
		for (i = 0; i < popped; i++) {
			msg = &msgs[i];
			if (msg->type == NDB_INGEST_QUIT)
				goto cleanup;
		}

		for (i = 0; i < popped; i++) {
			msg = &msgs[i];
			switch (msg->type) {
			case NDB_INGEST_QUIT:
				// quits are handled before this
				ndb_debug("ingester: unexpected quit message\n");
				goto cleanup;
			case NDB_INGEST_EVENT:
				out = &outs[to_write++];
				ndb_ingester_process_event(ctx, ingester,
							   &msg->event, out);
			}
		}

		if (to_write > 0)
			ndb_writer_queue_msgs(ingester->writer, outs, to_write);
	}

cleanup:
	ndb_debug("quitting ingester thread\n");
	secp256k1_context_destroy(ctx);
	return NULL;
}


static int ndb_writer_init(struct ndb_writer *writer)
{
	// make the queue big!
	writer->queue_buflen = sizeof(struct ndb_writer_msg) * DEFAULT_QUEUE_SIZE;
	writer->queue_buf = malloc(writer->queue_buflen);
	if (writer->queue_buf == NULL) {
		fprintf(stderr, "ndb: failed to allocate space for writer queue");
		return 0;
	}

	// init the writer queue.
	prot_queue_init(&writer->inbox, writer->queue_buf,
			writer->queue_buflen, sizeof(struct ndb_writer_msg));

	// spin up the writer thread
	if (pthread_create(&writer->thread_id, NULL, ndb_writer_thread, writer))
	{
		fprintf(stderr, "ndb writer thread failed to create\n");
		return 0;
	}
	
	return 1;
}

// initialize the ingester queue and then spawn the thread
static int ndb_ingester_init(struct ndb_ingester *ingester,
			     struct ndb_writer *writer, int num_threads)
{
	int elem_size, num_elems;
	static struct ndb_ingester_msg quit_msg = { .type = NDB_INGEST_QUIT };

	// TODO: configurable queue sizes
	elem_size = sizeof(struct ndb_ingester_msg);
	num_elems = DEFAULT_QUEUE_SIZE;

	ingester->writer = writer;

	if (!threadpool_init(&ingester->tp, num_threads, elem_size, num_elems,
			     &quit_msg, ingester, ndb_ingester_thread))
	{
		fprintf(stderr, "ndb ingester threadpool failed to init\n");
		return 0;
	}

	return 1;
}

static int ndb_writer_destroy(struct ndb_writer *writer)
{
	struct ndb_writer_msg msg;

	// kill thread
	msg.type = NDB_WRITER_QUIT;
	if (!prot_queue_push(&writer->inbox, &msg)) {
		// queue is too full to push quit message. just kill it.
		pthread_exit(writer->thread_id);
	} else {
		pthread_join(writer->thread_id, NULL);
	}

	// cleanup
	prot_queue_destroy(&writer->inbox);

	free(writer->queue_buf);

	return 1;
}

static int ndb_ingester_destroy(struct ndb_ingester *ingester)
{
	threadpool_destroy(&ingester->tp);
	return 1;
}

static int ndb_ingester_queue_event(struct ndb_ingester *ingester,
				    char *json, int len)
{
	struct ndb_ingester_msg msg;
	msg.type = NDB_INGEST_EVENT;

	msg.event.json = json;
	msg.event.len = len;

	return threadpool_dispatch(&ingester->tp, &msg);
}

static void ndb_make_id_ts(unsigned char *id, uint32_t created,
			   struct ndb_id_ts *ts)
{
	memcpy(ts->id, id, 32);
	ts->created = created;
}

int ndb_init(struct ndb **pndb, size_t mapsize, int ingester_threads)
{
	struct ndb *ndb;
	//MDB_dbi ind_id; // TODO: ind_pk, etc
	int rc;

	ndb = *pndb = calloc(1, sizeof(struct ndb));
	if (ndb == NULL) {
		fprintf(stderr, "ndb_init: malloc failed\n");
		return 0;
	}

	if ((rc = mdb_env_create(&ndb->env))) {
		fprintf(stderr, "mdb_env_create failed, error %d\n", rc);
		return 0;
	}

	if ((rc = mdb_env_set_mapsize(ndb->env, mapsize))) {
		fprintf(stderr, "mdb_env_set_mapsize failed, error %d\n", rc);
		return 0;
	}

	if ((rc = mdb_env_open(ndb->env, "./testdata/db", 0, 0664))) {
		fprintf(stderr, "mdb_env_open failed, error %d\n", rc);
		return 0;
	}

	if (!ndb_writer_init(&ndb->writer))
		return 0;

	if (!ndb_ingester_init(&ndb->ingester, &ndb->writer, ingester_threads))
		return 0;

	// Initialize LMDB environment and spin up threads
	return 1;
}

void ndb_destroy(struct ndb *ndb)
{
	if (ndb == NULL)
		return;

	mdb_env_close(ndb->env);

	// ingester depends on writer and must be destroyed first
	ndb_ingester_destroy(&ndb->ingester);
	ndb_writer_destroy(&ndb->writer);

	free(ndb);
}

// Process a nostr event, ie: ["EVENT", "subid", {"content":"..."}...]
// 
// This function returns as soon as possible, first copying the passed
// json and then queueing it up for processing. Worker threads then take
// the json and process it.
//
// Processing:
//
// 1. The event is parsed into ndb_notes and the signature is validated
// 2. A quick lookup is made on the database to see if we already have
//    the note id, if we do we don't need to waste time on json parsing
//    or note validation.
// 3. Once validation is done we pass it to the writer queue for writing
//    to LMDB.
//
int ndb_process_event(struct ndb *ndb, const char *json, int json_len)
{
	// Since we need to return as soon as possible, and we're not
	// making any assumptions about the lifetime of the string, we
	// definitely need to copy the json here. In the future once we
	// have our thread that manages a websocket connection, we can
	// avoid the copy and just use the buffer we get from that
	// thread.
	char *json_copy = strdupn(json, json_len);
	if (json_copy == NULL)
		return 0;

	ndb_ingester_queue_event(&ndb->ingester, json_copy, json_len);
	return 1;
}

static inline int cursor_push_tag(struct cursor *cur, struct ndb_tag *tag)
{
	return cursor_push_u16(cur, tag->count);
}

int ndb_builder_init(struct ndb_builder *builder, unsigned char *buf,
		     int bufsize)
{
	struct ndb_note *note;
	int half, size, str_indices_size;

	// come on bruh
	if (bufsize < sizeof(struct ndb_note) * 2)
		return 0;

	str_indices_size = bufsize / 32;
	size = bufsize - str_indices_size;
	half = size / 2;

	//debug("size %d half %d str_indices %d\n", size, half, str_indices_size);

	// make a safe cursor of our available memory
	make_cursor(buf, buf + bufsize, &builder->mem);

	note = builder->note = (struct ndb_note *)buf;

	// take slices of the memory into subcursors
	if (!(cursor_slice(&builder->mem, &builder->note_cur, half) &&
	      cursor_slice(&builder->mem, &builder->strings, half) &&
	      cursor_slice(&builder->mem, &builder->str_indices, str_indices_size))) {
		return 0;
	}

	memset(note, 0, sizeof(*note));
	builder->note_cur.p += sizeof(*note);

	note->strings = builder->strings.start - buf;
	note->version = 1;

	return 1;
}



static inline int ndb_json_parser_init(struct ndb_json_parser *p,
				       const char *json, int json_len,
				       unsigned char *buf, int bufsize)
{
	int half = bufsize / 2;

	unsigned char *tok_start = buf + half;
	unsigned char *tok_end = buf + bufsize;

	p->toks = (jsmntok_t*)tok_start;
	p->toks_end = (jsmntok_t*)tok_end;
	p->num_tokens = 0;
	p->json = json;
	p->json_len = json_len;

	// ndb_builder gets the first half of the buffer, and jsmn gets the
	// second half. I like this way of alloating memory (without actually
	// dynamically allocating memory). You get one big chunk upfront and
	// then submodules can recursively subdivide it. Maybe you could do
	// something even more clever like golden-ratio style subdivision where
	// the more important stuff gets a larger chunk and then it spirals
	// downward into smaller chunks. Thanks for coming to my TED talk.

	if (!ndb_builder_init(&p->builder, buf, half))
		return 0;

	jsmn_init(&p->json_parser);

	return 1;
}

static inline int ndb_json_parser_parse(struct ndb_json_parser *p)
{
	int cap = ((unsigned char *)p->toks_end - (unsigned char*)p->toks)/sizeof(*p->toks);
	p->num_tokens =
		jsmn_parse(&p->json_parser, p->json, p->json_len, p->toks, cap);

	p->i = 0;

	return p->num_tokens;
}

static int cursor_push_unescaped_char(struct cursor *cur, char c1, char c2)
{
	switch (c2) {
	case 't':  return cursor_push_byte(cur, '\t');
	case 'n':  return cursor_push_byte(cur, '\n');
	case 'r':  return cursor_push_byte(cur, '\r');
	case 'b':  return cursor_push_byte(cur, '\b');
	case 'f':  return cursor_push_byte(cur, '\f');
	case '\\': return cursor_push_byte(cur, '\\');
	case '"':  return cursor_push_byte(cur, '"');
	case 'u':
		// these aren't handled yet
		return 0;
	default:
		return cursor_push_byte(cur, c1) && cursor_push_byte(cur, c2);
	}
}

static int cursor_push_escaped_char(struct cursor *cur, char c)
{
        switch (c) {
        case '"':  return cursor_push_str(cur, "\\\"");
        case '\\': return cursor_push_str(cur, "\\\\");
        case '\b': return cursor_push_str(cur, "\\b");
        case '\f': return cursor_push_str(cur, "\\f");
        case '\n': return cursor_push_str(cur, "\\n");
        case '\r': return cursor_push_str(cur, "\\r");
        case '\t': return cursor_push_str(cur, "\\t");
        // TODO: \u hex hex hex hex
        }
        return cursor_push_byte(cur, c);
}

static int cursor_push_hex_str(struct cursor *cur, unsigned char *buf, int len)
{
	int i;

	if (len % 2 != 0)
		return 0;

        if (!cursor_push_byte(cur, '"'))
                return 0;

	for (i = 0; i < len; i++) {
		unsigned int c = ((const unsigned char *)buf)[i];
		if (!cursor_push_byte(cur, hexchar(c >> 4)))
			return 0;
		if (!cursor_push_byte(cur, hexchar(c & 0xF)))
			return 0;
	}

        if (!cursor_push_byte(cur, '"'))
                return 0;

	return 1;
}

static int cursor_push_jsonstr(struct cursor *cur, const char *str)
{
	int i;
        int len;

	len = strlen(str);

        if (!cursor_push_byte(cur, '"'))
                return 0;

        for (i = 0; i < len; i++) {
                if (!cursor_push_escaped_char(cur, str[i]))
                        return 0;
        }

        if (!cursor_push_byte(cur, '"'))
                return 0;

        return 1;
}


static inline int cursor_push_json_tag_str(struct cursor *cur, struct ndb_str str)
{
	if (str.flag == NDB_PACKED_ID)
		return cursor_push_hex_str(cur, str.id, 32);

	return cursor_push_jsonstr(cur, str.str);
}

static int cursor_push_json_tag(struct cursor *cur, struct ndb_note *note,
				struct ndb_tag *tag)
{
        int i;

        if (!cursor_push_byte(cur, '['))
                return 0;

        for (i = 0; i < tag->count; i++) {
                if (!cursor_push_json_tag_str(cur, ndb_note_str(note, &tag->strs[i])))
                        return 0;
                if (i != tag->count-1 && !cursor_push_byte(cur, ','))
			return 0;
        }

        return cursor_push_byte(cur, ']');
}

static int cursor_push_json_tags(struct cursor *cur, struct ndb_note *note)
{
	int i;
	struct ndb_iterator iter, *it = &iter;
	ndb_tags_iterate_start(note, it);

        if (!cursor_push_byte(cur, '['))
                return 0;

	i = 0;
	while (ndb_tags_iterate_next(it)) {
		if (!cursor_push_json_tag(cur, note, it->tag))
			return 0;
                if (i != note->tags.count-1 && !cursor_push_str(cur, ","))
			return 0;
		i++;
	}

        if (!cursor_push_byte(cur, ']'))
                return 0;

	return 1;
}

static int ndb_event_commitment(struct ndb_note *ev, unsigned char *buf, int buflen)
{
	char timebuf[16] = {0};
	char kindbuf[16] = {0};
	char pubkey[65];
	struct cursor cur;
	int ok;

	if (!hex_encode(ev->pubkey, sizeof(ev->pubkey), pubkey, sizeof(pubkey)))
		return 0;

	make_cursor(buf, buf + buflen, &cur);

	snprintf(timebuf, sizeof(timebuf), "%d", ev->created_at);
	snprintf(kindbuf, sizeof(kindbuf), "%d", ev->kind);

	ok =
		cursor_push_str(&cur, "[0,\"") &&
		cursor_push_str(&cur, pubkey) &&
		cursor_push_str(&cur, "\",") &&
		cursor_push_str(&cur, timebuf) &&
		cursor_push_str(&cur, ",") &&
		cursor_push_str(&cur, kindbuf) &&
		cursor_push_str(&cur, ",") &&
		cursor_push_json_tags(&cur, ev) &&
		cursor_push_str(&cur, ",") &&
		cursor_push_jsonstr(&cur, ndb_note_str(ev, &ev->content).str) &&
		cursor_push_str(&cur, "]");

	if (!ok)
		return 0;

	return cur.p - cur.start;
}

int ndb_calculate_id(struct ndb_note *note, unsigned char *buf, int buflen) {
	int len;

	if (!(len = ndb_event_commitment(note, buf, buflen)))
		return 0;

	//fprintf(stderr, "%.*s\n", len, buf);

	sha256((struct sha256*)note->id, buf, len);

	return 1;
}

int ndb_sign_id(struct ndb_keypair *keypair, unsigned char id[32],
		unsigned char sig[64])
{
	unsigned char aux[32];
	secp256k1_keypair *pair = (secp256k1_keypair*) keypair->pair;

	if (!fill_random(aux, sizeof(aux)))
		return 0;

	secp256k1_context *ctx =
		secp256k1_context_create(SECP256K1_CONTEXT_NONE);

	return secp256k1_schnorrsig_sign32(ctx, sig, id, pair, aux);
}

int ndb_create_keypair(struct ndb_keypair *kp)
{
	secp256k1_keypair *keypair = (secp256k1_keypair*)kp->pair;
	secp256k1_xonly_pubkey pubkey;

	secp256k1_context *ctx =
		secp256k1_context_create(SECP256K1_CONTEXT_NONE);;

	/* Try to create a keypair with a valid context, it should only
	 * fail if the secret key is zero or out of range. */
	if (!secp256k1_keypair_create(ctx, keypair, kp->secret))
		return 0;

	if (!secp256k1_keypair_xonly_pub(ctx, &pubkey, NULL, keypair))
		return 0;

	/* Serialize the public key. Should always return 1 for a valid public key. */
	return secp256k1_xonly_pubkey_serialize(ctx, kp->pubkey, &pubkey);
}

int ndb_decode_key(const char *secstr, struct ndb_keypair *keypair)
{
	if (!hex_decode(secstr, strlen(secstr), keypair->secret, 32)) {
		fprintf(stderr, "could not hex decode secret key\n");
		return 0;
	}

	return ndb_create_keypair(keypair);
}

int ndb_builder_finalize(struct ndb_builder *builder, struct ndb_note **note,
			 struct ndb_keypair *keypair)
{
	int strings_len = builder->strings.p - builder->strings.start;
	unsigned char *note_end = builder->note_cur.p + strings_len;
	int total_size = note_end - builder->note_cur.start;

	// move the strings buffer next to the end of our ndb_note
	memmove(builder->note_cur.p, builder->strings.start, strings_len);

	// set the strings location
	builder->note->strings = builder->note_cur.p - builder->note_cur.start;

	// record the total size
	//builder->note->size = total_size;

	*note = builder->note;

	// generate id and sign if we're building this manually
	if (keypair) {
		// use the remaining memory for building our id buffer
		unsigned char *end   = builder->mem.end;
		unsigned char *start = (unsigned char*)(*note) + total_size;

		ndb_builder_set_pubkey(builder, keypair->pubkey);

		if (!ndb_calculate_id(builder->note, start, end - start))
			return 0;

		if (!ndb_sign_id(keypair, (*note)->id, (*note)->sig))
			return 0;
	}

	return total_size;
}

struct ndb_note * ndb_builder_note(struct ndb_builder *builder)
{
	return builder->note;
}

/// find an existing string via str_indices. these indices only exist in the
/// builder phase just for this purpose.
static inline int ndb_builder_find_str(struct ndb_builder *builder,
				       const char *str, int len,
				       union ndb_packed_str *pstr)
{
	// find existing matching string to avoid duplicate strings
	int indices = cursor_count(&builder->str_indices, sizeof(uint32_t));
	for (int i = 0; i < indices; i++) {
		uint32_t index = ((uint32_t*)builder->str_indices.start)[i];
		const char *some_str = (const char*)builder->strings.start + index;

		if (!memcmp(some_str, str, len)) {
			// found an existing matching str, use that index
			*pstr = ndb_offset_str(index);
			return 1;
		}
	}

	return 0;
}

static int ndb_builder_push_str(struct ndb_builder *builder, const char *str,
				int len, union ndb_packed_str *pstr)
{
	uint32_t loc;

	// no string found, push a new one
	loc = builder->strings.p - builder->strings.start;
	if (!(cursor_push(&builder->strings, (unsigned char*)str, len) &&
	      cursor_push_byte(&builder->strings, '\0'))) {
		return 0;
	}

	*pstr = ndb_offset_str(loc);

	// record in builder indices. ignore return value, if we can't cache it
	// then whatever
	cursor_push_u32(&builder->str_indices, loc);

	return 1;
}

static int ndb_builder_push_packed_id(struct ndb_builder *builder,
				      unsigned char *id,
				      union ndb_packed_str *pstr)
{
	// Don't both find id duplicates. very rarely are they duplicated
	// and it slows things down quite a bit. If we really care about this
	// We can switch to a hash table.
	//if (ndb_builder_find_str(builder, (const char*)id, 32, pstr)) {
	//	pstr->packed.flag = NDB_PACKED_ID;
	//	return 1;
	//}

	if (ndb_builder_push_str(builder, (const char*)id, 32, pstr)) {
		pstr->packed.flag = NDB_PACKED_ID;
		return 1;
	}

	return 0;
}


/// Check for small strings to pack
static inline int ndb_builder_try_compact_str(struct ndb_builder *builder,
					      const char *str, int len,
					      union ndb_packed_str *pstr,
					      int pack_ids)
{
	unsigned char id_buf[32];

	if (len == 0) {
		*pstr = ndb_char_to_packed_str(0);
		return 1;
	} else if (len == 1) {
		*pstr = ndb_char_to_packed_str(str[0]);
		return 1;
	} else if (len == 2) {
		*pstr = ndb_chars_to_packed_str(str[0], str[1]);
		return 1;
	} else if (pack_ids && len == 64 && hex_decode(str, 64, id_buf, 32)) {
		return ndb_builder_push_packed_id(builder, id_buf, pstr);
	}

	return 0;
}


static int ndb_builder_push_unpacked_str(struct ndb_builder *builder,
					 const char *str, int len,
					 union ndb_packed_str *pstr)
{
	if (ndb_builder_find_str(builder, str, len, pstr))
		return 1;

	return ndb_builder_push_str(builder, str, len, pstr);
}

int ndb_builder_make_str(struct ndb_builder *builder, const char *str, int len,
			 union ndb_packed_str *pstr, int pack_ids)
{
	if (ndb_builder_try_compact_str(builder, str, len, pstr, pack_ids))
		return 1;

	return ndb_builder_push_unpacked_str(builder, str, len, pstr);
}

int ndb_builder_set_content(struct ndb_builder *builder, const char *content,
			    int len)
{
	int pack_ids = 0;
	builder->note->content_length = len;
	return ndb_builder_make_str(builder, content, len,
				    &builder->note->content, pack_ids);
}


static inline int jsoneq(const char *json, jsmntok_t *tok, int tok_len,
			 const char *s)
{
	if (tok->type == JSMN_STRING && (int)strlen(s) == tok_len &&
	    memcmp(json + tok->start, s, tok_len) == 0) {
		return 1;
	}
	return 0;
}

static inline int toksize(jsmntok_t *tok)
{
	return tok->end - tok->start;
}

static int ndb_builder_finalize_tag(struct ndb_builder *builder,
				    union ndb_packed_str offset)
{
	if (!cursor_push_u32(&builder->note_cur, offset.offset))
		return 0;
	builder->current_tag->count++;
	return 1;
}

/// Unescape and push json strings
static int ndb_builder_make_json_str(struct ndb_builder *builder,
				     const char *str, int len,
				     union ndb_packed_str *pstr,
				     int *written, int pack_ids)
{
	// let's not care about de-duping these. we should just unescape
	// in-place directly into the strings table. 
	if (written)
		*written = len;

	const char *p, *end, *start;
	unsigned char *builder_start;

	// always try compact strings first
	if (ndb_builder_try_compact_str(builder, str, len, pstr, pack_ids))
		return 1;

	end = str + len;
	start = str; // Initialize start to the beginning of the string

	*pstr = ndb_offset_str(builder->strings.p - builder->strings.start);
	builder_start = builder->strings.p;

	for (p = str; p < end; p++) {
		if (*p == '\\' && p+1 < end) {
			// Push the chunk of unescaped characters before this escape sequence
			if (start < p && !cursor_push(&builder->strings,
						(unsigned char *)start,
						p - start)) {
				return 0;
			}

			if (!cursor_push_unescaped_char(&builder->strings, *p, *(p+1)))
				return 0;

			p++; // Skip the character following the backslash
			start = p + 1; // Update the start pointer to the next character
		}
	}

	// Handle the last chunk after the last escape sequence (or if there are no escape sequences at all)
	if (start < p && !cursor_push(&builder->strings, (unsigned char *)start,
				      p - start)) {
		return 0;
	}

	if (written)
		*written = builder->strings.p - builder_start;

	// TODO: dedupe these!?
	return cursor_push_byte(&builder->strings, '\0');
}

static int ndb_builder_push_json_tag(struct ndb_builder *builder,
				     const char *str, int len)
{
	union ndb_packed_str pstr;
	int pack_ids = 1;
	if (!ndb_builder_make_json_str(builder, str, len, &pstr, NULL, pack_ids))
		return 0;
	return ndb_builder_finalize_tag(builder, pstr);
}

// Push a json array into an ndb tag ["p", "abcd..."] -> struct ndb_tag
static int ndb_builder_tag_from_json_array(struct ndb_json_parser *p,
					   jsmntok_t *array)
{
	jsmntok_t *str_tok;
	const char *str;

	if (array->size == 0)
		return 0;

	if (!ndb_builder_new_tag(&p->builder))
		return 0;

	for (int i = 0; i < array->size; i++) {
		str_tok = &array[i+1];
		str = p->json + str_tok->start;

		if (!ndb_builder_push_json_tag(&p->builder, str,
					       toksize(str_tok))) {
			return 0;
		}
	}

	return 1;
}

// Push json tags into ndb data
//   [["t", "hashtag"], ["p", "abcde..."]] -> struct ndb_tags
static inline int ndb_builder_process_json_tags(struct ndb_json_parser *p,
						jsmntok_t *array)
{
	jsmntok_t *tag = array;

	if (array->size == 0)
		return 1;

	for (int i = 0; i < array->size; i++) {
		if (!ndb_builder_tag_from_json_array(p, &tag[i+1]))
			return 0;
		tag += tag[i+1].size;
	}

	return 1;
}

static int parse_unsigned_int(const char *start, int len, unsigned int *num)
{
	unsigned int number = 0;
	const char *p = start, *end = start + len;
	int digits = 0;

	while (p < end) {
		char c = *p;

		if (c < '0' || c > '9')
			break;

		// Check for overflow
		char digit = c - '0';
		if (number > (UINT_MAX - digit) / 10)
			return 0; // Overflow detected

		number = number * 10 + digit;

		p++;
		digits++;
	}

	if (digits == 0)
		return 0;

	*num = number;
	return 1;
}

int ndb_ws_event_from_json(const char *json, int len, struct ndb_tce *tce,
			   unsigned char *buf, int bufsize)
{
	jsmntok_t *tok = NULL;
	int tok_len, res;
	struct ndb_json_parser parser;

	tce->subid_len = 0;
	tce->subid = "";

	ndb_json_parser_init(&parser, json, len, buf, bufsize);
	if ((res = ndb_json_parser_parse(&parser)) < 0)
		return res;

	if (parser.num_tokens < 3 || parser.toks[0].type != JSMN_ARRAY)
		return 0;

	parser.i = 1;
	tok = &parser.toks[parser.i++];
	tok_len = toksize(tok);
	if (tok->type != JSMN_STRING)
		return 0;

	if (tok_len == 5 && !memcmp("EVENT", json + tok->start, 5)) {
		tce->evtype = NDB_TCE_EVENT;
		struct ndb_event *ev = &tce->event;

		tok = &parser.toks[parser.i++];
		if (tok->type != JSMN_STRING)
			return 0;

		tce->subid = json + tok->start;
		tce->subid_len = toksize(tok);

		return ndb_parse_json_note(&parser, &ev->note);
	} else if (tok_len == 4 && !memcmp("EOSE", json + tok->start, 4)) { 
		tce->evtype = NDB_TCE_EOSE;

		tok = &parser.toks[parser.i++];
		if (tok->type != JSMN_STRING)
			return 0;

		tce->subid = json + tok->start;
		tce->subid_len = toksize(tok);
		return 1;
	} else if (tok_len == 2 && !memcmp("OK", json + tok->start, 2)) {
		if (parser.num_tokens != 5)
			return 0;

		struct ndb_command_result *cr = &tce->command_result;

		tce->evtype = NDB_TCE_OK;

		tok = &parser.toks[parser.i++];
		if (tok->type != JSMN_STRING)
			return 0;

		tce->subid = json + tok->start;
		tce->subid_len = toksize(tok);

		tok = &parser.toks[parser.i++];
		if (tok->type != JSMN_PRIMITIVE || toksize(tok) == 0)
			return 0;

		cr->ok = (json + tok->start)[0] == 't';

		tok = &parser.toks[parser.i++];
		if (tok->type != JSMN_STRING)
			return 0;

		tce->command_result.msg = json + tok->start;
		tce->command_result.msglen = toksize(tok);

		return 1;
	}

	return 0;
}

int ndb_parse_json_note(struct ndb_json_parser *parser, struct ndb_note **note)
{
	jsmntok_t *tok = NULL;
	unsigned char hexbuf[64];
	const char *json = parser->json;
	const char *start;
	int i, tok_len;

	if (parser->toks[parser->i].type != JSMN_OBJECT)
		return 0;

	// TODO: build id buffer and verify at end

	for (i = parser->i + 1; i < parser->num_tokens; i++) {
		tok = &parser->toks[i];
		start = json + tok->start;
		tok_len = toksize(tok);

		//printf("toplevel %.*s %d\n", tok_len, json + tok->start, tok->type);
		if (tok_len == 0 || i + 1 >= parser->num_tokens)
			continue;

		if (start[0] == 'p' && jsoneq(json, tok, tok_len, "pubkey")) {
			// pubkey
			tok = &parser->toks[i+1];
			hex_decode(json + tok->start, toksize(tok), hexbuf, sizeof(hexbuf));
			ndb_builder_set_pubkey(&parser->builder, hexbuf);
		} else if (tok_len == 2 && start[0] == 'i' && start[1] == 'd') {
			// id
			tok = &parser->toks[i+1];
			hex_decode(json + tok->start, toksize(tok), hexbuf, sizeof(hexbuf));
			// TODO: validate id
			ndb_builder_set_id(&parser->builder, hexbuf);
		} else if (tok_len == 3 && start[0] == 's' && start[1] == 'i' && start[2] == 'g') {
			// sig
			tok = &parser->toks[i+1];
			hex_decode(json + tok->start, toksize(tok), hexbuf, sizeof(hexbuf));
			ndb_builder_set_sig(&parser->builder, hexbuf);
		} else if (start[0] == 'k' && jsoneq(json, tok, tok_len, "kind")) {
			// kind
			tok = &parser->toks[i+1];
			start = json + tok->start;
			if (tok->type != JSMN_PRIMITIVE || tok_len <= 0)
				return 0;
			if (!parse_unsigned_int(start, toksize(tok),
						&parser->builder.note->kind))
					return 0;
		} else if (start[0] == 'c') {
			if (jsoneq(json, tok, tok_len, "created_at")) {
				// created_at
				tok = &parser->toks[i+1];
				start = json + tok->start;
				if (tok->type != JSMN_PRIMITIVE || tok_len <= 0)
					return 0;
				if (!parse_unsigned_int(start, toksize(tok),
							&parser->builder.note->created_at))
					return 0;
			} else if (jsoneq(json, tok, tok_len, "content")) {
				// content
				tok = &parser->toks[i+1];
				union ndb_packed_str pstr;
				tok_len = toksize(tok);
				int written, pack_ids = 0;
				if (!ndb_builder_make_json_str(&parser->builder,
							json + tok->start,
							tok_len, &pstr,
							&written, pack_ids)) {
					return 0;
				}
				parser->builder.note->content_length = written;
				parser->builder.note->content = pstr;
			}
		} else if (start[0] == 't' && jsoneq(json, tok, tok_len, "tags")) {
			tok = &parser->toks[i+1];
			ndb_builder_process_json_tags(parser, tok);
			i += tok->size;
		}
	}

	return ndb_builder_finalize(&parser->builder, note, NULL);
}

int ndb_note_from_json(const char *json, int len, struct ndb_note **note,
		       unsigned char *buf, int bufsize)
{
	struct ndb_json_parser parser;
	int res;

	ndb_json_parser_init(&parser, json, len, buf, bufsize);
	if ((res = ndb_json_parser_parse(&parser)) < 0)
		return res;

	if (parser.num_tokens < 1)
		return 0;

	return ndb_parse_json_note(&parser, note);
}

void ndb_builder_set_pubkey(struct ndb_builder *builder, unsigned char *pubkey)
{
	memcpy(builder->note->pubkey, pubkey, 32);
}

void ndb_builder_set_id(struct ndb_builder *builder, unsigned char *id)
{
	memcpy(builder->note->id, id, 32);
}

void ndb_builder_set_sig(struct ndb_builder *builder, unsigned char *sig)
{
	memcpy(builder->note->sig, sig, 64);
}

void ndb_builder_set_kind(struct ndb_builder *builder, uint32_t kind)
{
	builder->note->kind = kind;
}

void ndb_builder_set_created_at(struct ndb_builder *builder, uint32_t created_at)
{
	builder->note->created_at = created_at;
}

int ndb_builder_new_tag(struct ndb_builder *builder)
{
	builder->note->tags.count++;
	struct ndb_tag tag = {0};
	builder->current_tag = (struct ndb_tag *)builder->note_cur.p;
	return cursor_push_tag(&builder->note_cur, &tag);
}

/// Push an element to the current tag
/// 
/// Basic idea is to call ndb_builder_new_tag
inline int ndb_builder_push_tag_str(struct ndb_builder *builder,
				    const char *str, int len)
{
	union ndb_packed_str pstr;
	int pack_ids = 1;
	if (!ndb_builder_make_str(builder, str, len, &pstr, pack_ids))
		return 0;
	return ndb_builder_finalize_tag(builder, pstr);
}
