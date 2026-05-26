//
//  wasm.c
//  sqlite-wasm
//
//  Created by Gioele Cantoni on 25/06/25.
//


#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <emscripten/fetch.h>
#include <emscripten/emscripten.h>
#include "sqlite3.h"

// sqlite-sync extension
#define CLOUDSYNC_OMIT_CURL
#include "modules/sqlite-sync/src/network/network_private.h"
#include "modules/sqlite-sync/src/utils.c"
#include "modules/sqlite-sync/src/network/network.c"
#include "modules/sqlite-sync/src/dbutils.c"
#include "modules/sqlite-sync/src/cloudsync.c"
#include "modules/sqlite-sync/src/pk.c"
#include "modules/sqlite-sync/src/lz4.c"
#include "modules/sqlite-sync/modules/fractional-indexing/fractional_indexing.c"
#include "modules/sqlite-sync/src/block.c"
#include "modules/sqlite-sync/src/sqlite/cloudsync_changes_sqlite.c"
#include "modules/sqlite-sync/src/sqlite/cloudsync_sqlite.c"
#include "modules/sqlite-sync/src/sqlite/database_sqlite.c"
#include "modules/sqlite-sync/src/sqlite/sql_sqlite.c"

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

#define SQLITEAI_WASM_WRAPPER_VERSION "1.0.0"
#define AUTH_HEADER_MAXSIZE 4096

char *substr(const char *start, const char *end) {
    size_t len = end - start;
    char *out = (char *)malloc(len + 1);
    if (out) {
        memcpy(out, start, len);
        out[len] = 0;
    }
    return out;
}

static bool wasm_header_name_equals(const char *start, const char *end, const char *name) {
    size_t name_len = strlen(name);
    if ((size_t)(end - start) != name_len) return false;

    for (size_t i = 0; i < name_len; i++) {
        if (tolower((unsigned char)start[i]) != tolower((unsigned char)name[i])) return false;
    }

    return true;
}

static char *wasm_response_header_dup(emscripten_fetch_t *fetch, const char *name) {
    if (!fetch || !name) return NULL;

    size_t headers_len = emscripten_fetch_get_response_headers_length(fetch);
    if (headers_len == 0) return NULL;

    char *headers = (char *)malloc(headers_len + 1);
    if (!headers) return NULL;

    if (emscripten_fetch_get_response_headers(fetch, headers, headers_len + 1) == 0) {
        free(headers);
        return NULL;
    }

    char *value = NULL;
    const char *line = headers;
    while (*line) {
        const char *line_end = strchr(line, '\n');
        if (!line_end) line_end = line + strlen(line);

        const char *colon = memchr(line, ':', (size_t)(line_end - line));
        if (colon && wasm_header_name_equals(line, colon, name)) {
            const char *value_start = colon + 1;
            const char *value_end = line_end;

            while (value_start < value_end && (*value_start == ' ' || *value_start == '\t')) value_start++;
            while (value_end > value_start && (value_end[-1] == '\r' || value_end[-1] == '\n' || value_end[-1] == ' ' || value_end[-1] == '\t')) value_end--;

            size_t value_len = (size_t)(value_end - value_start);
            value = (char *)malloc(value_len + 1);
            if (value) {
                memcpy(value, value_start, value_len);
                value[value_len] = 0;
            }
            break;
        }

        line = (*line_end == '\n') ? line_end + 1 : line_end;
    }

    free(headers);
    return value;
}

static void wasm_request_headers_free(char **header_keys, int allocated_keys, const char **headers) {
    for (int i = 0; i < allocated_keys; i++) free(header_keys[i]);
    free(header_keys);
    free(headers);
}

NETWORK_RESULT network_receive_buffer (network_data *data, const char *endpoint, const char *authentication, bool zero_terminated, bool is_post_request, char *json_payload, const char **extra_headers, int nextra_headers) {
    char *buffer = NULL;
    size_t blen = 0;
    bool using_ticket = network_data_should_use_ticket(data, endpoint, authentication);

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
    int max_header_pairs = nextra_headers + 5;
    const char **headers = (const char **)calloc((size_t)(max_header_pairs * 2 + 1), sizeof(char *));
    char **header_keys = (char **)calloc((size_t)(nextra_headers > 0 ? nextra_headers : 1), sizeof(char *));
    if (!headers || !header_keys) {
        free(headers);
        free(header_keys);
        return (NETWORK_RESULT){CLOUDSYNC_NETWORK_ERROR, NULL, 0, NULL, NULL};
    }

    int h = 0;
    int allocated_keys = 0;

    // Extra headers arrive as "Key: Value"; Emscripten fetch expects key/value pairs.
    for (int i = 0; i < nextra_headers; i++) {
        const char *header = extra_headers ? extra_headers[i] : NULL;
        if (!header) continue;

        const char *colon = strchr(header, ':');
        if (colon) {
            size_t klen = (size_t)(colon - header);
            char *key = (char *)malloc(klen + 1);
            if (!key) {
                wasm_request_headers_free(header_keys, allocated_keys, headers);
                return (NETWORK_RESULT){CLOUDSYNC_NETWORK_ERROR, NULL, 0, NULL, NULL};
            }
            memcpy(key, header, klen);
            key[klen] = 0;

            const char *value = colon + 1;
            while (*value == ' ') value++;

            header_keys[allocated_keys++] = key;
            headers[h++] = key;
            headers[h++] = value;
        }
    }

    // Organization
    char org_header[512];
    char *org_id = network_data_get_orgid(data);
    if (org_id) {
        snprintf(org_header, sizeof(org_header), "%s", org_id);
        headers[h++] = CLOUDSYNC_HEADER_ORG;
        headers[h++] = org_header;
    }

    // Authorization
    char auth_header[AUTH_HEADER_MAXSIZE];
    if (authentication) {
        snprintf(auth_header, sizeof(auth_header), "Bearer %s", authentication);
        headers[h++] = "Authorization";
        headers[h++] = auth_header;
    }

    // CloudSync session ticket
    char ticket_header[CLOUDSYNC_SESSION_TOKEN_MAXSIZE];
    if (using_ticket) {
        char *ticket = network_data_get_ticket(data);
        if (strlen(ticket) < sizeof(ticket_header)) {
            snprintf(ticket_header, sizeof(ticket_header), "%s", ticket);
            headers[h++] = CLOUDSYNC_HEADER_TICKET;
            headers[h++] = ticket_header;
        }
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
    if (!fetch) {
        result.code = CLOUDSYNC_NETWORK_ERROR;
        wasm_request_headers_free(header_keys, allocated_keys, headers);
        return result;
    }

    if(fetch->readyState == 4){
        if (fetch->numBytes > SIZE_MAX) {
            result.code = CLOUDSYNC_NETWORK_ERROR;
            emscripten_fetch_close(fetch);
            wasm_request_headers_free(header_keys, allocated_keys, headers);
            return result;
        }
        buffer = (char *)fetch->data;
        blen = (size_t)fetch->numBytes;
    }

    if (fetch->status >= 200 && fetch->status < 400) {
        char *ticket = wasm_response_header_dup(fetch, CLOUDSYNC_HEADER_TICKET);
        if (ticket && ticket[0]) {
            char *expires_at = wasm_response_header_dup(fetch, CLOUDSYNC_HEADER_TICKET_EXPIRES_AT);
            network_data_update_ticket(data, ticket, expires_at);
            free(expires_at);
        }
        free(ticket);
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
        if (fetch->statusText[0]) {
            result.buffer = strdup(fetch->statusText);
            result.blen = strlen(fetch->statusText);
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
    wasm_request_headers_free(header_keys, allocated_keys, headers);

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
    char auth_header[AUTH_HEADER_MAXSIZE];
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
    if (!fetch) return false;
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
    char                *api_key;
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

static int dbmem_json_skip_token (const jsmntok_t *tokens, int index) {
    int next = index + 1;

    if (tokens[index].type == JSMN_ARRAY) {
        for (int i = 0; i < tokens[index].size; i++) {
            next = dbmem_json_skip_token(tokens, next);
        }
        return next;
    }

    if (tokens[index].type == JSMN_OBJECT) {
        for (int i = 0; i < tokens[index].size; i++) {
            next += 1; // skip key token
            next = dbmem_json_skip_token(tokens, next);
        }
        return next;
    }

    return next;
}

static bool dbmem_json_token_equals (const char *json, const jsmntok_t *token, const char *text) {
    size_t len = strlen(text);
    size_t token_len = (size_t)(token->end - token->start);
    return token_len == len && memcmp(json + token->start, text, len) == 0;
}

static int dbmem_json_object_find (const char *json, const jsmntok_t *tokens, int object_index, const char *key) {
    if (object_index < 0 || tokens[object_index].type != JSMN_OBJECT) return -1;

    int index = object_index + 1;
    for (int i = 0; i < tokens[object_index].size; i++) {
        int key_index = index;
        int value_index = key_index + 1;

        if (tokens[key_index].type != JSMN_STRING) return -1;
        if (dbmem_json_token_equals(json, &tokens[key_index], key)) return value_index;

        index = dbmem_json_skip_token(tokens, value_index);
    }

    return -1;
}

static bool dbmem_json_parse_bool (const char *json, const jsmntok_t *token) {
    size_t len = (size_t)(token->end - token->start);
    return token->type == JSMN_PRIMITIVE && len == 4 && memcmp(json + token->start, "true", 4) == 0;
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

    char *_api_key = dbmem_strdup(api_key);
    if (!_api_key) {
        snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to duplicate API key (insufficient memory)");
        dbmem_remote_engine_free(engine);
        dbmem_free(request);
        dbmem_free(data);
        dbmem_free(_provider);
        dbmem_free(_model);
        return NULL;
    }

    engine->context = (dbmem_context *)ctx;
    engine->api_key = _api_key;
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
    char auth_header[512];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", engine->api_key);

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
    int n_embd = 0;
    int request_tokens = 0;
    bool truncated = false;
    int emb_start = -1;
    size_t emb_count = 0;

    if (tokens[0].type != JSMN_OBJECT) {
        dbmem_context_set_error(engine->context, "Invalid API response shape");
        return -1;
    }

    int output_dimension_index = dbmem_json_object_find(engine->data, tokens, 0, "output_dimension");
    if (output_dimension_index >= 0 && tokens[output_dimension_index].type == JSMN_PRIMITIVE) {
        n_embd = atoi(engine->data + tokens[output_dimension_index].start);
    }

    int data_index = dbmem_json_object_find(engine->data, tokens, 0, "data");
    if (data_index < 0 || tokens[data_index].type != JSMN_ARRAY || tokens[data_index].size <= 0) {
        dbmem_context_set_error(engine->context, "Missing embedding data in API response");
        return -1;
    }

    int item_index = data_index + 1;
    if (tokens[item_index].type != JSMN_OBJECT) {
        dbmem_context_set_error(engine->context, "Invalid embedding item in API response");
        return -1;
    }

    int embedding_index = dbmem_json_object_find(engine->data, tokens, item_index, "embedding");
    if (embedding_index < 0 || tokens[embedding_index].type != JSMN_ARRAY) {
        dbmem_context_set_error(engine->context, "Missing embedding data in API response");
        return -1;
    }
    if (tokens[embedding_index].size <= 0) {
        dbmem_context_set_error(engine->context, "Invalid embedding array size in API response");
        return -1;
    }
    emb_count = (size_t)tokens[embedding_index].size;
    emb_start = embedding_index + 1;

    int truncated_index = dbmem_json_object_find(engine->data, tokens, item_index, "truncated");
    if (truncated_index >= 0) {
        truncated = dbmem_json_parse_bool(engine->data, &tokens[truncated_index]);
    }

    int usage_index = dbmem_json_object_find(engine->data, tokens, 0, "usage");
    if (usage_index >= 0 && tokens[usage_index].type == JSMN_OBJECT) {
        int request_tokens_index = dbmem_json_object_find(engine->data, tokens, usage_index, "request_tokens");
        if (request_tokens_index >= 0 && tokens[request_tokens_index].type == JSMN_PRIMITIVE) {
            request_tokens = atoi(engine->data + tokens[request_tokens_index].start);
        }
    }

    if (n_embd == 0 && emb_count > 0) {
        n_embd = (int)emb_count;
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

    for (size_t i = 0; i < emb_count; i++) {
        engine->embedding[i] = strtof(engine->data + tokens[emb_start + i].start, NULL);
    }

    result->n_embd = n_embd;
    result->n_tokens = request_tokens;
    result->truncated = truncated;
    result->embedding = engine->embedding;

    engine->total_tokens_processed += result->n_tokens;
    engine->total_embeddings_generated++;

    return 0;
}

int dbmem_remote_engine_set_apikey (dbmem_remote_engine_t *engine, const char *api_key, char err_msg[DBMEM_ERRBUF_SIZE]) {
    if (!engine || !api_key) {
        if (err_msg) snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Invalid remote engine or API key");
        return SQLITE_MISUSE;
    }

    char *copy = dbmem_strdup(api_key);
    if (!copy) {
        if (err_msg) snprintf(err_msg, DBMEM_ERRBUF_SIZE, "Unable to duplicate API key (insufficient memory)");
        return SQLITE_NOMEM;
    }

    if (engine->api_key) dbmem_free(engine->api_key);
    engine->api_key = copy;

    return SQLITE_OK;
}

void dbmem_remote_engine_free (dbmem_remote_engine_t *engine) {
    if (!engine) return;
    if (engine->api_key) dbmem_free(engine->api_key);
    if (engine->provider) dbmem_free(engine->provider);
    if (engine->model) dbmem_free(engine->model);
    if (engine->data) dbmem_free(engine->data);
    if (engine->request) dbmem_free(engine->request);
    if (engine->embedding) dbmem_free(engine->embedding);
    if (engine->tokens) dbmem_free(engine->tokens);
    dbmem_free(engine);
}

// MARK: -

static void sqliteai_wasm_version(sqlite3_context *context, int argc, sqlite3_value **argv) {
    (void)argc;
    (void)argv;
    sqlite3_result_text(context, SQLITEAI_WASM_WRAPPER_VERSION, -1, SQLITE_STATIC);
}

static int sqlite3_wasm_wrapper_init(sqlite3 *db, char **pzErrMsg, const sqlite3_api_routines *pApi) {
    (void)pzErrMsg;
    (void)pApi;
    return sqlite3_create_function(db, "wasm_version", 0, SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, sqliteai_wasm_version, NULL, NULL);
}

int sqlite3_wasm_extra_init(const char *z) {
    fprintf(stderr, "%s: %s()\n", __FILE__, __func__);
    int rc = SQLITE_OK;
    rc = sqlite3_auto_extension((void *) sqlite3_wasm_wrapper_init);
    rc = sqlite3_auto_extension((void *) sqlite3_cloudsync_init);
    rc = sqlite3_auto_extension((void *) sqlite3_vector_init);
    rc = sqlite3_auto_extension((void *) sqlite3_memory_init);
    return rc;
}
