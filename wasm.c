//
//  wasm.c
//  sqlite-wasm
//
//  Created by Gioele Cantoni on 25/06/25.
//


#include <stdio.h>
#include <stdlib.h>
#include <emscripten/fetch.h>
#include <emscripten/emscripten.h>
#include "sqlite3.h"

// sqlite-sync extension
#define CLOUDSYNC_OMIT_CURL
#include "modules/sqlite-sync/src/network_private.h"
#include "modules/sqlite-sync/src/utils.c"
#include "modules/sqlite-sync/src/network.c"
#include "modules/sqlite-sync/src/dbutils.c"
#include "modules/sqlite-sync/src/cloudsync.c"
#include "modules/sqlite-sync/src/vtab.c"
#include "modules/sqlite-sync/src/pk.c"
#include "modules/sqlite-sync/src/lz4.c"

// sqlite-vector extension
#include "modules/sqlite-vector/src/sqlite-vector.c"
#include "modules/sqlite-vector/src/distance-sse2.c"
#include "modules/sqlite-vector/src/distance-neon.c"
#include "modules/sqlite-vector/src/distance-cpu.c"
#include "modules/sqlite-vector/src/distance-avx2.c"
#include "modules/sqlite-vector/src/distance-avx512.c"

// sqlite-memory extension
#define DBMEM_OMIT_LOCAL_ENGINE
#define DBMEM_OMIT_IO
#include "modules/sqlite-memory/src/dbmem-utils.c"
#include "modules/sqlite-memory/src/dbmem-parser.c"
#include "modules/sqlite-memory/src/dbmem-search.c"
#include "modules/sqlite-memory/src/md4c.c"
#include "modules/sqlite-memory/src/sqlite-memory.c"

// MARK: - WASM -

char *substr(const char *start, const char *end) {
    size_t len = end - start;
    char *out = (char *)malloc(len + 1);
    if (out) {
        memcpy(out, start, len);
        out[len] = 0;
    }
    return out;
}

NETWORK_RESULT network_receive_buffer (network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char *custom_header) {
    char *buffer = NULL;
    size_t blen = 0;

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);

    // Set method
    if (json_payload || is_post_request) {
        strcpy(attr.requestMethod, "POST");
    } else {
        strcpy(attr.requestMethod, "GET");
    }
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS | EMSCRIPTEN_FETCH_REPLACE;

    // Prepare header array (alternating key, value, NULL-terminated)
    const char *headers[11];
    int h = 0;

    // Custom header (must be "Key: Value", split at ':')
    char *custom_key = NULL;
    if (custom_header) {
        const char *colon = strchr(custom_header, ':');
        if (colon) {
            size_t klen = colon - custom_header;
            custom_key = (char *)malloc(klen + 1);
            strncpy(custom_key, custom_header, klen);
            custom_key[klen] = 0;
            const char *custom_val = colon + 1;
            while (*custom_val == ' ') custom_val++;
            headers[h++] = custom_key;
            headers[h++] = custom_val;
        }
    }

    // Authorization
    char auth_header[256];
    if (authentication) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", authentication);
        headers[h++] = "Authorization";
        headers[h++] = auth_header;
    }

    // Content-Type for JSON
    if (json_payload) {
        headers[h++] = "Content-Type";
        headers[h++] = "application/json";
    }
    
    headers[h] = 0;
    attr.requestHeaders = headers;

    // Body
    if (json_payload) {
        attr.requestData = json_payload;
        attr.requestDataSize = strlen(json_payload);
    }

    emscripten_fetch_t *fetch = emscripten_fetch(&attr, endpoint); // Blocks here until the operation is complete.
    NETWORK_RESULT result = {0, NULL, 0, NULL, NULL};

    if(fetch->readyState == 4){
        buffer = fetch->data;
        blen = fetch->totalBytes;
    }
    
    if (fetch->status >= 200 && fetch->status < 300) {
        if (blen > 0 && buffer) {
            char *buf = (char*)malloc(blen + 1);
            if (buf) {
                memcpy(buf, buffer, blen);
                buf[blen] = 0;
                result.code = CLOUDSYNC_NETWORK_BUFFER;
                result.buffer = buf;
                result.blen = blen;
                result.xfree = free;
            } else result.code = CLOUDSYNC_NETWORK_ERROR;
        } else result.code = CLOUDSYNC_NETWORK_OK;
    } else {
        result.code = CLOUDSYNC_NETWORK_ERROR;
        if (fetch->statusText && fetch->statusText[0]) {
            result.buffer = strdup(fetch->statusText);
            result.blen = sizeof(fetch->statusText);
            result.xfree = free;
        } else if (blen > 0 && buffer) {
            char *buf = (char*)malloc(blen + 1);
            if (buf) {
                memcpy(buf, buffer, blen);
                buf[blen] = 0;
                result.buffer = buf;
                result.blen = blen;
                result.xfree = free;
            }
        }
    }

    // cleanup
    emscripten_fetch_close(fetch);
    if (custom_key) free(custom_key);

    return result;
}

bool network_send_buffer(network_data *data, const char *endpoint, const char *authentication, const void *blob, int blob_size) {

    bool result = false;
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "PUT");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS | EMSCRIPTEN_FETCH_REPLACE;

    // Prepare headers (alternating key, value, NULL-terminated)
    // Max 3 headers: Accept, (optional Auth), Content-Type
    const char *headers[7];
    int h = 0;
    headers[h++] = "Accept";
    headers[h++] = "text/plain";
    char auth_header[256];
    if (authentication) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", authentication);
        headers[h++] = "Authorization";
        headers[h++] = auth_header;
    }
    headers[h++] = "Content-Type";
    headers[h++] = "application/octet-stream";
    headers[h] = 0;
    attr.requestHeaders = headers;

    // Set request body
    attr.requestData = (const char *)blob;
    attr.requestDataSize = blob_size;

    emscripten_fetch_t *fetch = emscripten_fetch(&attr, endpoint); // Blocks here until the operation is complete.
    if (fetch->status >= 200 && fetch->status < 300) result = true;

    emscripten_fetch_close(fetch);

    return result;
}

// MARK: - sqlite-memory WASM remote embedding engine -

#include "modules/sqlite-memory/src/dbmem-embed.h"
#include "modules/sqlite-memory/src/jsmn.h"

#define API_URL             "https://api.vectors.space/v1/embeddings"
#define DEFAULT_BUFFER_SIZE (100*1024)

struct dbmem_remote_engine_t {
    dbmem_context       *context;
    char                *provider;
    char                *model;
    char                *data;          size_t data_capacity, data_size;
    char                *request;       size_t request_capacity;
    float               *embedding;    size_t embedding_capacity;
    jsmntok_t           *tokens;       int tokens_capacity;
    int64_t             total_tokens_processed;
    int64_t             total_embeddings_generated;
};

static bool text_needs_json_escape (const char *text, size_t *len) {
    size_t original_len = *len;
    size_t required_len = 0;
    bool needs_escape = false;

    for (size_t i = 0; i < original_len; i++) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
            case '"': case '\\': case '\b': case '\f': case '\n': case '\r': case '\t':
                required_len += 2;
                needs_escape = true;
                break;
            default:
                if (c < 0x20) { required_len += 6; needs_escape = true; }
                else { required_len += 1; }
        }
    }
    *len = required_len;
    return needs_escape;
}

static size_t text_encode_json (char *buffer, size_t buffer_size, const char *text, size_t text_len) {
    UNUSED_PARAM(buffer_size);
    char *p = buffer;
    for (size_t i = 0; i < text_len; i++) {
        unsigned char c = (unsigned char)text[i];
        switch (c) {
            case '"':  *p++ = '\\'; *p++ = '"';  break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\b': *p++ = '\\'; *p++ = 'b';  break;
            case '\f': *p++ = '\\'; *p++ = 'f';  break;
            case '\n': *p++ = '\\'; *p++ = 'n';  break;
            case '\r': *p++ = '\\'; *p++ = 'r';  break;
            case '\t': *p++ = '\\'; *p++ = 't';  break;
            default:
                if (c < 0x20) {
                    static const char hex[] = "0123456789abcdef";
                    *p++ = '\\'; *p++ = 'u'; *p++ = '0'; *p++ = '0';
                    *p++ = hex[(c >> 4) & 0xF]; *p++ = hex[c & 0xF];
                } else { *p++ = c; }
        }
    }
    *p = '\0';
    return (size_t)(p - buffer);
}

static int set_json_error_message (dbmem_remote_engine_t *engine) {
    const char *errmsg = "Unknown API error";
    jsmn_parser parser;
    jsmntok_t tokens[16];
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, engine->data, engine->data_size, tokens, 16);
    for (int i = 0; i < ntokens - 1; i++) {
        if (tokens[i].type == JSMN_STRING && tokens[i].end - tokens[i].start == 7 && memcmp(engine->data + tokens[i].start, "message", 7) == 0) {
            jsmntok_t *val = &tokens[i + 1];
            engine->data[val->end] = '\0';
            errmsg = engine->data + val->start;
            break;
        }
    }
    dbmem_context_set_error(engine->context, errmsg);
    return -1;
}

dbmem_remote_engine_t *dbmem_remote_engine_init (void *ctx, const char *provider, const char *model, char err_msg[DBMEM_ERRBUF_SIZE]) {
    const char *api_key = dbmem_context_apikey((dbmem_context *)ctx);
    if (!api_key) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "memory_set_apikey must be called before requesting remote embedding");
        return NULL;
    }

    dbmem_remote_engine_t *engine = (dbmem_remote_engine_t *)dbmem_zeroalloc(sizeof(dbmem_remote_engine_t));
    if (!engine) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to allocate memory for the remote embedding engine");
        return NULL;
    }

    char *data = dbmem_alloc(DEFAULT_BUFFER_SIZE);
    if (!data) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to allocate memory for the default buffer (1)");
        dbmem_remote_engine_free(engine);
        return NULL;
    }

    char *request = dbmem_alloc(DEFAULT_BUFFER_SIZE);
    if (!request) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to allocate memory for the default buffer (2)");
        dbmem_remote_engine_free(engine);
        dbmem_free(data);
        return NULL;
    }

    char *_provider = dbmem_strdup(provider);
    if (!_provider) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to duplicate provider name (insufficient memory)");
        dbmem_remote_engine_free(engine);
        dbmem_free(request);
        dbmem_free(data);
        return NULL;
    }

    char *_model = dbmem_strdup(model);
    if (!_model) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to duplicate model name (insufficient memory)");
        dbmem_remote_engine_free(engine);
        dbmem_free(request);
        dbmem_free(data);
        dbmem_free(_provider);
        return NULL;
    }

    engine->context = (dbmem_context *)ctx;
    engine->provider = _provider;
    engine->model = _model;
    engine->data = data;
    engine->request = request;
    engine->data_capacity = DEFAULT_BUFFER_SIZE;
    engine->request_capacity = DEFAULT_BUFFER_SIZE;

    return engine;
}

int dbmem_remote_compute_embedding (dbmem_remote_engine_t *engine, const char *text, int text_len, embedding_result_t *result) {
    engine->data_size = 0;

    // check if text needs JSON encoding
    size_t len = (size_t)text_len;
    bool encoding_needed = text_needs_json_escape(text, &len);

    // grow request buffer if needed
    size_t provider_len = strlen(engine->provider);
    size_t model_len = strlen(engine->model);
    if (engine->request_capacity < len + provider_len + model_len + 128) {
        size_t new_size = len + provider_len + model_len + 1024;
        char *new_request = dbmem_alloc(new_size);
        if (!new_request) {
            dbmem_context_set_error(engine->context, "Unable to allocate request buffer");
            return -1;
        }
        dbmem_free(engine->request);
        engine->request = new_request;
        engine->request_capacity = new_size;
    }

    // build JSON request
    if (encoding_needed) {
        int seek = snprintf(engine->request, engine->request_capacity, "{\"provider\": \"%s\", \"model\": \"%s\", \"input\": \"", engine->provider, engine->model);
        size_t seek2 = text_encode_json(engine->request+seek, (engine->request_capacity - seek), text, text_len);
        snprintf(engine->request+seek+seek2, (engine->request_capacity - seek - seek2), "\",\"strategy\": {\"type\": \"truncate\"}}");
    } else {
        snprintf(engine->request, engine->request_capacity,
                 "{\"provider\": \"%s\",\"model\": \"%s\",\"input\": \"%s\",\"strategy\": {\"type\": \"truncate\"}}",
                 engine->provider, engine->model, text);
    }

    // perform HTTP request via emscripten fetch
    const char *api_key = dbmem_context_apikey(engine->context);
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    strcpy(attr.requestMethod, "POST");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS | EMSCRIPTEN_FETCH_REPLACE;

    const char *headers[] = {
        "Authorization", auth_header,
        "Content-Type", "application/json",
        0
    };
    attr.requestHeaders = headers;
    attr.requestData = engine->request;
    attr.requestDataSize = strlen(engine->request);

    emscripten_fetch_t *fetch = emscripten_fetch(&attr, API_URL);

    if (fetch->status != 200) {
        // copy response data for error parsing
        if (fetch->numBytes > 0 && fetch->data) {
            size_t blen = fetch->numBytes;
            if (blen + 1 > engine->data_capacity) {
                char *new_data = dbmem_alloc(blen + 1);
                if (new_data) {
                    dbmem_free(engine->data);
                    engine->data = new_data;
                    engine->data_capacity = blen + 1;
                }
            }
            if (blen < engine->data_capacity) {
                memcpy(engine->data, fetch->data, blen);
                engine->data[blen] = '\0';
                engine->data_size = blen;
            }
        }
        emscripten_fetch_close(fetch);
        return set_json_error_message(engine);
    }

    // copy response into data buffer
    size_t blen = fetch->numBytes;
    if (blen + 1 > engine->data_capacity) {
        size_t new_capacity = (blen + 1) * 2;
        char *new_data = dbmem_alloc(new_capacity);
        if (!new_data) {
            emscripten_fetch_close(fetch);
            dbmem_context_set_error(engine->context, "Unable to allocate response buffer");
            return -1;
        }
        dbmem_free(engine->data);
        engine->data = new_data;
        engine->data_capacity = new_capacity;
    }
    memcpy(engine->data, fetch->data, blen);
    engine->data[blen] = '\0';
    engine->data_size = blen;
    emscripten_fetch_close(fetch);

    // parse JSON response (two-pass: count tokens, then parse)
    jsmn_parser parser;
    jsmn_init(&parser);
    int ntokens = jsmn_parse(&parser, engine->data, engine->data_size, NULL, 0);
    if (ntokens < 1) {
        dbmem_context_set_error(engine->context, "Failed to parse API response");
        return -1;
    }

    if (engine->tokens_capacity < ntokens) {
        if (engine->tokens) dbmem_free(engine->tokens);
        engine->tokens = (jsmntok_t *)dbmem_alloc(sizeof(jsmntok_t) * ntokens);
        if (!engine->tokens) {
            dbmem_context_set_error(engine->context, "Unable to allocate JSON tokens");
            return -1;
        }
        engine->tokens_capacity = ntokens;
    }

    jsmn_init(&parser);
    jsmn_parse(&parser, engine->data, engine->data_size, engine->tokens, ntokens);
    jsmntok_t *tokens = engine->tokens;

    // extract fields
    int n_embd = 0, prompt_tokens = 0, estimated_prompt_tokens = 0;
    int emb_start = -1, emb_count = 0;

    for (int i = 0; i < ntokens - 1; i++) {
        if (tokens[i].type != JSMN_STRING) continue;
        int klen = tokens[i].end - tokens[i].start;
        const char *key = engine->data + tokens[i].start;

        if (klen == 9 && memcmp(key, "embedding", 9) == 0 && tokens[i + 1].type == JSMN_ARRAY) {
            emb_count = tokens[i + 1].size;
            emb_start = i + 2;
        } else if (klen == 16 && memcmp(key, "output_dimension", 16) == 0) {
            n_embd = atoi(engine->data + tokens[i + 1].start);
        } else if (klen == 13 && memcmp(key, "prompt_tokens", 13) == 0 && tokens[i + 1].type == JSMN_PRIMITIVE) {
            prompt_tokens = atoi(engine->data + tokens[i + 1].start);
        } else if (klen == 23 && memcmp(key, "estimated_prompt_tokens", 23) == 0) {
            estimated_prompt_tokens = atoi(engine->data + tokens[i + 1].start);
        }
    }

    if (emb_start < 0 || emb_count == 0 || n_embd == 0) {
        dbmem_context_set_error(engine->context, "Missing embedding data in API response");
        return -1;
    }

    if (engine->embedding_capacity < (size_t)emb_count) {
        if (engine->embedding) dbmem_free(engine->embedding);
        engine->embedding = (float *)dbmem_alloc(sizeof(float) * emb_count);
        if (!engine->embedding) {
            dbmem_context_set_error(engine->context, "Unable to allocate embedding buffer");
            return -1;
        }
        engine->embedding_capacity = emb_count;
    }

    for (int i = 0; i < emb_count; i++) {
        engine->embedding[i] = strtof(engine->data + tokens[emb_start + i].start, NULL);
    }

    result->n_embd = n_embd;
    result->n_tokens = prompt_tokens;
    result->n_tokens_truncated = (estimated_prompt_tokens > prompt_tokens) ? estimated_prompt_tokens - prompt_tokens : 0;
    result->embedding = engine->embedding;

    engine->total_tokens_processed += prompt_tokens;
    engine->total_embeddings_generated++;

    return 0;
}

void dbmem_remote_engine_free (dbmem_remote_engine_t *engine) {
    if (!engine) return;
    if (engine->provider) dbmem_free(engine->provider);
    if (engine->model) dbmem_free(engine->model);
    if (engine->data) dbmem_free(engine->data);
    if (engine->request) dbmem_free(engine->request);
    if (engine->embedding) dbmem_free(engine->embedding);
    if (engine->tokens) dbmem_free(engine->tokens);
    dbmem_free(engine);
}

// MARK: -

int sqlite3_wasm_extra_init(const char *z) {
    fprintf(stderr, "%s: %s()\n", __FILE__, __func__);
    int rc = SQLITE_OK;
    rc = sqlite3_auto_extension((void *) sqlite3_cloudsync_init);
    rc = sqlite3_auto_extension((void *) sqlite3_vector_init);
    rc = sqlite3_auto_extension((void *) sqlite3_memory_init);
    return rc;
}