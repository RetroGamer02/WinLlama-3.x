/*
 * WinLlama 3.x - Win16 Winsock 1.1 connectivity milestone.
 *
 * Keep this source compatible with OpenWatcom's Windows 3.1 target:
 * no Win32-only APIs, no Winsock 2, and no near-pointer assumptions.
 *
 * MEMORY MODEL NOTE: the four big buffers below (request/response/body/
 * context) are NOT static arrays -- they're allocated with GlobalAlloc()
 * and addressed with __huge pointers, living entirely outside DGROUP
 * (the 64KB default data segment that holds near statics + the stack).
 * __huge specifically (not just __far) is what lets a single object be
 * bigger than 64KB: plain __far pointer arithmetic silently wraps at
 * each 64KB segment boundary instead of advancing into the next one;
 * __huge pointers get compiler-generated normalization on every
 * increment/dereference so they don't. The cost is that Watcom's
 * standard library string functions (strstr, strncmp, strlen) only
 * understand near pointers in this small-model build, so a handful of
 * tiny custom Huge* helpers below replace them for anything that
 * touches these buffers. Only g_server/g_port_text/g_model/g_prompt
 * stay as ordinary near statics -- they're small and always will be.
 */
#include <windows.h>
#include <winsock.h>
#include <stdlib.h>
#include <string.h>

#include "winllama.h"

#define SERVER_TEXT_SIZE 128
#define PORT_TEXT_SIZE   8
#define MODEL_TEXT_SIZE  64
#define PROMPT_TEXT_SIZE 768
#define STATUS_TEXT_SIZE 192

/* Per-recv()/send() chunk size. Winsock 1.1's recv/send take a 16-bit
   SIGNED int length (max +32767); asking for "everything remaining" in
   one call would silently misbehave once that remaining amount exceeds
   32767, which the old fixed-size buffers never triggered but a 512KB
   one absolutely would. Keeping each individual call small and looping
   sidesteps that entirely, and also keeps every huge-pointer-based
   destination address safely within a single 64KB segment for the
   duration of that one call. */
#define IO_CHUNK_SIZE    4096

/* "Generous" tier: a 512KB response buffer and ~60KB of conversation
   memory, per your choice. RESPONSE_SIZE is the one buffer that
   actually needs __huge (it's far bigger than one 64KB segment).
   CONTEXT_SIZE is kept a hair under 64KB on purpose so BODY_SIZE and
   REQUEST_SIZE (which both have to hold a full copy of it) also fit in
   a single segment -- avoiding needing __huge for those two as well. */
#define RESPONSE_SIZE    (512UL * 1024UL)
#define CONTEXT_SIZE     61440UL
#define BODY_SIZE        ((unsigned long)PROMPT_TEXT_SIZE + MODEL_TEXT_SIZE + CONTEXT_SIZE + 96UL)
#define REQUEST_SIZE     (BODY_SIZE + 256UL)

/* Win 3.1's own multi-line edit control has a practical text-length
   ceiling well under our new response capacity (it tracks selection
   and length internally with 16-bit values), so a genuinely huge reply
   still can't just be handed to SetDlgItemText() in full. This caps
   what we display, with a note, rather than risking the control choke
   on a very long string. */
#define SAFE_DISPLAY_LIMIT 30000UL

static HINSTANCE g_instance;

static char huge *g_request;
static char huge *g_response;
static char huge *g_body;
static char huge *g_context;
static HGLOBAL g_request_handle;
static HGLOBAL g_response_handle;
static HGLOBAL g_body_handle;
static HGLOBAL g_context_handle;

/*
 * Win16 apps run on a single, small per-task stack shared with every
 * nested USER/GDI call (including the WM_PAINT reentrancy that
 * UpdateWindow() triggers from inside SetStatus()). These stay as
 * ordinary near statics rather than locals for the same reason the big
 * buffers were originally moved off the stack: keeping automatic
 * (stack) storage small on the Send path is what fixed the original
 * crash-on-Send bug. None of these functions are reentrant, so static
 * storage is safe here.
 */
static char g_server[SERVER_TEXT_SIZE];
static char g_port_text[PORT_TEXT_SIZE];
static char g_model[MODEL_TEXT_SIZE];
static char g_prompt[PROMPT_TEXT_SIZE];


static char huge *FindResponseBody(char huge *http_response);

static void SetStatus(HWND dialog, const char *text)
{
    SetDlgItemText(dialog, IDC_STATUS, text);
    /* The synchronous first implementation must at least repaint its state. */
    UpdateWindow(dialog);
}

static void SetSocketError(HWND dialog, const char *operation)
{
    char text[STATUS_TEXT_SIZE];

    wsprintf(text, "%s failed (error %d).", operation, WSAGetLastError());
    SetStatus(dialog, text);
}

/*
 * Self-written replacements for strstr/strncmp/strlen: those standard
 * library functions only understand near pointers in this small-model
 * build, and our four big buffers live outside DGROUP now. needle/
 * prefix are always one of our own short literal keys, so they stay
 * ordinary near strings -- only the haystack side needs to be huge.
 */
static char huge *HugeFindText(const char huge *haystack, const char *needle)
{
    const char huge *h;
    const char huge *match_start;
    const char *n;

    for (h = haystack; *h != '\0'; ++h) {
        if (*h != needle[0]) {
            continue;
        }
        match_start = h;
        n = needle;
        while (*n != '\0' && *h != '\0' && *h == *n) {
            ++h;
            ++n;
        }
        if (*n == '\0') {
            return (char huge *)match_start;
        }
        h = match_start;
    }
    return NULL;
}

static BOOL HugeStartsWith(const char huge *text, const char *prefix)
{
    while (*prefix != '\0') {
        if (*text != *prefix) {
            return FALSE;
        }
        ++text;
        ++prefix;
    }
    return TRUE;
}

static unsigned long HugeStrLen(const char huge *text)
{
    unsigned long length;

    length = 0;
    while (*text != '\0') {
        ++text;
        ++length;
    }
    return length;
}

static BOOL AllocateBigBuffers(void)
{
    g_request_handle  = GlobalAlloc(GMEM_FIXED, (DWORD)REQUEST_SIZE);
    g_response_handle = GlobalAlloc(GMEM_FIXED, (DWORD)RESPONSE_SIZE);
    g_body_handle     = GlobalAlloc(GMEM_FIXED, (DWORD)BODY_SIZE);
    g_context_handle  = GlobalAlloc(GMEM_FIXED, (DWORD)CONTEXT_SIZE);
    if (g_request_handle == NULL || g_response_handle == NULL ||
        g_body_handle == NULL || g_context_handle == NULL) {
        return FALSE;
    }

    g_request  = (char huge *)GlobalLock(g_request_handle);
    g_response = (char huge *)GlobalLock(g_response_handle);
    g_body     = (char huge *)GlobalLock(g_body_handle);
    g_context  = (char huge *)GlobalLock(g_context_handle);
    if (g_request == NULL || g_response == NULL || g_body == NULL || g_context == NULL) {
        return FALSE;
    }

    g_request[0] = '\0';
    g_response[0] = '\0';
    g_body[0] = '\0';
    g_context[0] = '\0';
    return TRUE;
}

static void FreeBigBuffers(void)
{
    if (g_request_handle != NULL) {
        GlobalUnlock(g_request_handle);
        GlobalFree(g_request_handle);
    }
    if (g_response_handle != NULL) {
        GlobalUnlock(g_response_handle);
        GlobalFree(g_response_handle);
    }
    if (g_body_handle != NULL) {
        GlobalUnlock(g_body_handle);
        GlobalFree(g_body_handle);
    }
    if (g_context_handle != NULL) {
        GlobalUnlock(g_context_handle);
        GlobalFree(g_context_handle);
    }
}

static BOOL ResolveServer(const char *server, struct in_addr *address)
{
    unsigned long numeric_address;
    struct hostent FAR *host;

    numeric_address = inet_addr(server);
    if (numeric_address != INADDR_NONE) {
        address->s_addr = numeric_address;
        return TRUE;
    }

    host = gethostbyname(server);
    if (host == NULL || host->h_addrtype != AF_INET || host->h_length != 4) {
        return FALSE;
    }

    _fmemcpy(address, host->h_addr_list[0], sizeof(*address));
    return TRUE;
}

static BOOL StartWinsock(HWND dialog)
{
    WSADATA winsock_data;
    int startup_result;

    startup_result = WSAStartup((WORD)0x0101, &winsock_data);
    if (startup_result != 0) {
        char text[STATUS_TEXT_SIZE];
        wsprintf(text, "WSAStartup failed (%d).", startup_result);
        SetStatus(dialog, text);
        return FALSE;
    }

    if (LOBYTE(winsock_data.wVersion) != 1 || HIBYTE(winsock_data.wVersion) != 1) {
        SetStatus(dialog, "Winsock provider isn't v1.1.");
        WSACleanup();
        return FALSE;
    }

    return TRUE;
}

static SOCKET ConnectToServer(HWND dialog, const char *server, unsigned short port)
{
    struct sockaddr_in server_address;
    SOCKET socket_handle;

    SetStatus(dialog, "Resolving server...");
    if (!ResolveServer(server, &server_address.sin_addr)) {
        SetSocketError(dialog, "Name lookup");
        return INVALID_SOCKET;
    }

    socket_handle = socket(AF_INET, SOCK_STREAM, 0);
    if (socket_handle == INVALID_SOCKET) {
        SetSocketError(dialog, "socket");
        return INVALID_SOCKET;
    }

    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(port);
    SetStatus(dialog, "Connecting...");
    if (connect(socket_handle, (struct sockaddr FAR *)&server_address,
                sizeof(server_address)) == SOCKET_ERROR) {
        SetSocketError(dialog, "connect");
        closesocket(socket_handle);
        return INVALID_SOCKET;
    }

    return socket_handle;
}

static void ClearFields(HWND dialog)
{
    SetDlgItemText(dialog, IDC_PROMPT, "");
    SetDlgItemText(dialog, IDC_RESPONSE, "");
    g_context[0] = '\0';
    SetStatus(dialog, "Ready. Enter server and test.");
    SetFocus(GetDlgItem(dialog, IDC_PROMPT));
}

static void TestConnection(HWND dialog)
{
    SOCKET socket_handle;
    char text[STATUS_TEXT_SIZE];
    unsigned short port;

    GetDlgItemText(dialog, IDC_SERVER, g_server, sizeof(g_server));
    GetDlgItemText(dialog, IDC_PORT, g_port_text, sizeof(g_port_text));
    port = (unsigned short)atoi(g_port_text);

    if (g_server[0] == '\0' || port == 0) {
        SetStatus(dialog, "Enter a server/IP and valid port.");
        return;
    }

    SetStatus(dialog, "Starting Winsock 1.1...");
    if (!StartWinsock(dialog)) {
        return;
    }

    socket_handle = ConnectToServer(dialog, g_server, port);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return;
    }

    closesocket(socket_handle);
    WSACleanup();
    wsprintf(text, "Connected to %s:%u", g_server, (unsigned int)port);
    SetStatus(dialog, text);
}

/*
 * destination/remaining are declared __huge uniformly (rather than
 * having separate near/far/huge variants of every helper) because a
 * near or far pointer can always be safely widened to __huge -- so the
 * SAME functions work whether they're filling a small near buffer, one
 * of the far-but-under-64KB buffers, or the truly-huge response buffer.
 */
static BOOL AppendCharacter(char huge **destination, unsigned long *remaining, char character)
{
    if (*remaining <= 1) {
        return FALSE;
    }
    **destination = character;
    ++*destination;
    --*remaining;
    **destination = '\0';
    return TRUE;
}

static BOOL AppendText(char huge **destination, unsigned long *remaining, const char huge *text)
{
    while (*text != '\0') {
        if (!AppendCharacter(destination, remaining, *text++)) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL AppendUnsigned(char huge **destination, unsigned long *remaining, unsigned long value)
{
    char digits[12];
    unsigned int length;

    length = 0;
    do {
        digits[length++] = (char)('0' + (value % 10));
        value /= 10;
    } while (value != 0 && length < sizeof(digits));

    while (length != 0) {
        if (!AppendCharacter(destination, remaining, digits[--length])) {
            return FALSE;
        }
    }
    return TRUE;
}

static BOOL AppendJsonString(char huge **destination, unsigned long *remaining, const char huge *text)
{
    if (!AppendCharacter(destination, remaining, '"')) {
        return FALSE;
    }
    while (*text != '\0') {
        switch (*text) {
        case '"':
        case '\\':
            if (!AppendCharacter(destination, remaining, '\\') ||
                !AppendCharacter(destination, remaining, *text)) {
                return FALSE;
            }
            break;
        case '\n':
            if (!AppendText(destination, remaining, "\\n")) return FALSE;
            break;
        case '\r':
            if (!AppendText(destination, remaining, "\\r")) return FALSE;
            break;
        case '\t':
            if (!AppendText(destination, remaining, "\\t")) return FALSE;
            break;
        default:
            if (!AppendCharacter(destination, remaining, *text)) return FALSE;
            break;
        }
        ++text;
    }
    return AppendCharacter(destination, remaining, '"');
}

static BOOL BuildGenerateRequest(const char *model, const char *prompt, BOOL disable_thinking)
{
    char huge *body_cursor;
    char huge *request_cursor;
    unsigned long body_remaining;
    unsigned long request_remaining;

    body_cursor = g_body;
    body_remaining = BODY_SIZE;
    *body_cursor = '\0';
    if (!AppendText(&body_cursor, &body_remaining, "{\"model\":" ) ||
        !AppendJsonString(&body_cursor, &body_remaining, model) ||
        !AppendText(&body_cursor, &body_remaining, ",\"prompt\":" ) ||
        !AppendJsonString(&body_cursor, &body_remaining, prompt) ||
        !AppendText(&body_cursor, &body_remaining, ",\"think\":") ||
        !AppendText(&body_cursor, &body_remaining, disable_thinking ? "false" : "true")) {
        return FALSE;
    }
    if (g_context[0] != '\0') {
        /* Continues the previous exchange; see g_context's declaration. */
        if (!AppendText(&body_cursor, &body_remaining, ",\"context\":") ||
            !AppendText(&body_cursor, &body_remaining, g_context)) {
            return FALSE;
        }
    }
    if (!AppendText(&body_cursor, &body_remaining, ",\"stream\":false}")) {
        return FALSE;
    }

    request_cursor = g_request;
    request_remaining = REQUEST_SIZE;
    *request_cursor = '\0';
    return AppendText(&request_cursor, &request_remaining, "POST /api/generate HTTP/1.0\r\nContent-Type: application/json\r\nContent-Length: ") &&
           AppendUnsigned(&request_cursor, &request_remaining, HugeStrLen(g_body)) &&
           AppendText(&request_cursor, &request_remaining, "\r\nConnection: close\r\n\r\n") &&
           AppendText(&request_cursor, &request_remaining, g_body);
}

static BOOL SendAll(SOCKET socket_handle, const char huge *buffer, unsigned long length)
{
    unsigned long offset;
    unsigned int chunk;
    int sent;

    offset = 0;
    while (offset < length) {
        chunk = (unsigned int)((length - offset) > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : (length - offset));
        sent = send(socket_handle, (const char FAR *)(buffer + offset), (int)chunk, 0);
        if (sent == SOCKET_ERROR || sent == 0) {
            return FALSE;
        }
        offset += (unsigned long)sent;
    }
    return TRUE;
}

static long GetContentLength(const char huge *headers)
{
    const char huge *field;
    long value;
    BOOL found_digit;

    field = HugeFindText(headers, "\r\nContent-Length:");
    if (field == NULL) {
        return -1;
    }
    field += 17;
    while (*field == ' ' || *field == '\t') {
        ++field;
    }
    value = 0;
    found_digit = FALSE;
    while (*field >= '0' && *field <= '9') {
        value = value * 10 + (*field - '0');
        found_digit = TRUE;
        ++field;
    }
    return found_digit ? value : -1;
}

static BOOL IsChunkedResponse(const char huge *headers)
{
    return HugeFindText(headers, "\r\nTransfer-Encoding: chunked") != NULL;
}

typedef enum {
    RECEIVE_OK,
    RECEIVE_TOO_LARGE,
    RECEIVE_SOCKET_ERROR
} ReceiveStatus;

static ReceiveStatus ReceiveAll(SOCKET socket_handle, unsigned long *response_length)
{
    int received;
    unsigned long used;
    unsigned long remaining_capacity;
    unsigned int chunk;
    char huge *body;
    long content_length;

    used = 0;
    for (;;) {
        if (used >= RESPONSE_SIZE - 1) {
            g_response[used] = '\0';
            *response_length = used;
            return RECEIVE_TOO_LARGE;
        }
        remaining_capacity = RESPONSE_SIZE - 1 - used;
        chunk = (unsigned int)(remaining_capacity > IO_CHUNK_SIZE ? IO_CHUNK_SIZE : remaining_capacity);
        received = recv(socket_handle, (char FAR *)(g_response + used), (int)chunk, 0);
        if (received == 0) {
            g_response[used] = '\0';
            *response_length = used;
            return RECEIVE_OK;
        }
        if (received == SOCKET_ERROR) {
            return RECEIVE_SOCKET_ERROR;
        }
        used += (unsigned long)received;
        g_response[used] = '\0';

        /*
         * Ollama normally returns Content-Length for stream:false.  Do not
         * wait for a connection close after the whole body is already here.
         */
        body = FindResponseBody(g_response);
        if (body != NULL) {
            if (IsChunkedResponse(g_response) &&
                HugeFindText(body, "\r\n0\r\n\r\n") != NULL) {
                *response_length = used;
                return RECEIVE_OK;
            }
            content_length = GetContentLength(g_response);
            if (content_length >= 0 &&
                (long)used >= (long)(body - g_response) + content_length) {
                *response_length = used;
                return RECEIVE_OK;
            }
        }
    }
}

static char huge *FindResponseBody(char huge *http_response)
{
    char huge *cursor;

    for (cursor = http_response; cursor[0] != '\0'; ++cursor) {
        if (cursor[0] == '\r' && cursor[1] == '\n' &&
            cursor[2] == '\r' && cursor[3] == '\n') {
            return cursor + 4;
        }
    }
    return NULL;
}

static int HexValue(char character)
{
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'f') return character - 'a' + 10;
    if (character >= 'A' && character <= 'F') return character - 'A' + 10;
    return -1;
}

static BOOL DecodeChunkedBody(const char huge *encoded, char huge *decoded, unsigned long decoded_size)
{
    unsigned long chunk_length;
    unsigned long remaining;
    int digit;
    BOOL saw_digit;

    /*
     * Callers pass decoded == encoded to decode in place (see
     * GenerateResponse). That's safe as long as we never write to
     * *decoded before encoded has already read past that position --
     * chunk framing (size digits + CRLFs) is always consumed without
     * being written out, so the write cursor never catches up to the
     * read cursor. The one thing that would break this is writing the
     * initial '\0' terminator before any bytes have been consumed, so
     * that write is deferred below until after the first chunk header
     * has been parsed instead of happening up front.
     */
    remaining = decoded_size;
    for (;;) {
        chunk_length = 0;
        saw_digit = FALSE;
        while (*encoded != '\r' && *encoded != '\0' && *encoded != ';') {
            digit = HexValue(*encoded++);
            if (digit < 0) return FALSE;
            chunk_length = chunk_length * 16 + (unsigned long)digit;
            saw_digit = TRUE;
        }
        if (!saw_digit) return FALSE;
        while (*encoded != '\r' && *encoded != '\0') ++encoded;
        if (encoded[0] != '\r' || encoded[1] != '\n') return FALSE;
        encoded += 2;
        *decoded = '\0';
        if (chunk_length == 0) return TRUE;
        while (chunk_length != 0) {
            if (*encoded == '\0' || !AppendCharacter(&decoded, &remaining, *encoded++)) {
                return FALSE;
            }
            --chunk_length;
        }
        if (encoded[0] != '\r' || encoded[1] != '\n') return FALSE;
        encoded += 2;
    }
}

/*
 * Ollama is a Go program, and Go's JSON encoder emits any non-ASCII model
 * output as raw UTF-8 bytes rather than \uXXXX-escaping it, so this never
 * goes through the \u handling in ExtractJsonResponse below. Win16's ANSI
 * edit controls have no idea what UTF-8 is, so each byte of a multi-byte
 * sequence used to render as its own wrong glyph -- the classic mojibode
 * look. This decodes one UTF-8 sequence starting at cursor into a Unicode
 * codepoint, returning the number of bytes it consumed, or 0 if cursor
 * doesn't start a valid sequence (caller then falls back to treating that
 * one byte literally).
 */
static int DecodeUtf8Sequence(const char huge *cursor, unsigned long *codepoint)
{
    unsigned char lead;
    int extra;
    int i;
    unsigned long value;
    unsigned char cont;

    lead = (unsigned char)cursor[0];
    if (lead < 0x80) {
        *codepoint = lead;
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        extra = 1;
        value = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        extra = 2;
        value = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        extra = 3;
        value = lead & 0x07;
    } else {
        return 0;
    }
    for (i = 1; i <= extra; ++i) {
        cont = (unsigned char)cursor[i];
        if ((cont & 0xC0) != 0x80) {
            return 0;
        }
        value = (value << 6) | (cont & 0x3F);
    }
    *codepoint = value;
    return extra + 1;
}

/*
 * Best-effort ANSI stand-in for a Unicode codepoint decoded from raw UTF-8.
 * Plain Latin-1 code points pass through as a single byte (a close enough
 * match to Win 3.1's ANSI code page for accented letters); the handful of
 * "smart" typography characters LLMs love get an ASCII substitute so they
 * always render regardless of code page or font; anything else becomes
 * '?'. single_char_buffer must have room for 2 bytes and outlives only the
 * call site that supplied it.
 */
static const char *AnsiApproximation(unsigned long codepoint, char *single_char_buffer)
{
    switch (codepoint) {
    case 0x2018: case 0x2019: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2013: return "-";
    case 0x2014: return "--";
    case 0x2026: return "...";
    case 0x2022: return "*";
    case 0x2500: case 0x2501: return "-";
    case 0x2713: case 0x2714: return "v";
    case 0x2717: case 0x2718: return "x";
    default:
        if (codepoint <= 0xFF) {
            single_char_buffer[0] = (char)codepoint;
            single_char_buffer[1] = '\0';
            return single_char_buffer;
        }
        return "?";
    }
}

static BOOL ExtractJsonResponse(char huge *json, char huge *output, unsigned long output_size)
{
    static const char response_key[] = "\"response\":\"";
    char huge *cursor;
    unsigned long remaining;

    cursor = HugeFindText(json, response_key);
    if (cursor == NULL) {
        return FALSE;
    }
    cursor += sizeof(response_key) - 1;
    remaining = output_size;
    *output = '\0';
    while (*cursor != '\0' && *cursor != '"') {
        if (*cursor == '\\' && cursor[1] != '\0') {
            ++cursor;
            if (*cursor == 'n') *cursor = '\n';
            else if (*cursor == 'r') *cursor = '\r';
            else if (*cursor == 't') *cursor = '\t';
            else if (*cursor == 'u') {
                /*
                 * \uXXXX escapes are how JSON encodes '<', '>', etc.
                 * (e.g. reasoning models emitting <think> tags). Win16's
                 * ANSI text controls can't render arbitrary code points,
                 * so anything outside Latin-1 becomes '?'; everything
                 * else is decoded to its single-byte value in place.
                 * (unsigned int, not int: a 4-hex-digit value can reach
                 * 0xFFFF, which overflows a signed 16-bit int.)
                 */
                unsigned int codepoint = 0;
                int i;
                BOOL valid_escape = TRUE;
                for (i = 1; i <= 4; ++i) {
                    int digit = HexValue(cursor[i]);
                    if (digit < 0) {
                        valid_escape = FALSE;
                        break;
                    }
                    codepoint = (unsigned int)((codepoint << 4) | (unsigned int)digit);
                }
                if (valid_escape) {
                    cursor += 4;
                    *cursor = (codepoint <= 0xFF) ? (char)codepoint : '?';
                }
            }
            if (!AppendCharacter(&output, &remaining, *cursor++)) {
                return FALSE;
            }
            continue;
        }
        if ((unsigned char)*cursor >= 0x80) {
            unsigned long codepoint;
            int consumed = DecodeUtf8Sequence(cursor, &codepoint);
            if (consumed >= 1) {
                char single_char_buffer[2];
                const char *replacement = AnsiApproximation(codepoint, single_char_buffer);
                const char *r;
                for (r = replacement; *r != '\0'; ++r) {
                    if (!AppendCharacter(&output, &remaining, *r)) {
                        return FALSE;
                    }
                }
                cursor += consumed;
                continue;
            }
            /* Not a valid UTF-8 sequence; fall through and copy the byte
               as-is rather than getting stuck. */
        }
        if (!AppendCharacter(&output, &remaining, *cursor++)) {
            return FALSE;
        }
    }
    return *cursor == '"';
}

/*
 * Copies the "context":[...] array (Ollama's short conversational-memory
 * token list) out of a still-intact response JSON, brackets included, as
 * raw text -- there's no need to parse the numbers, just store them and
 * splice them back into the next request verbatim. Must be called BEFORE
 * ExtractJsonResponse() touches the same buffer: that function truncates
 * its "json" string in place at the end of the "response" value, and
 * "context" normally appears later in the object, so anything past that
 * point becomes invisible to string functions once it has run.
 */
static BOOL ExtractContextArray(const char huge *json, char huge *output, unsigned long output_size)
{
    static const char context_key[] = "\"context\":[";
    const char huge *cursor;
    unsigned long remaining;

    cursor = HugeFindText(json, context_key);
    if (cursor == NULL) {
        return FALSE;
    }
    cursor += sizeof(context_key) - 1;
    remaining = output_size;
    *output = '\0';
    if (!AppendCharacter(&output, &remaining, '[')) {
        return FALSE;
    }
    while (*cursor != ']' && *cursor != '\0') {
        if (!AppendCharacter(&output, &remaining, *cursor++)) {
            return FALSE;
        }
    }
    if (*cursor != ']') {
        return FALSE;
    }
    return AppendCharacter(&output, &remaining, ']');
}

static void GenerateResponse(HWND dialog)
{
    char huge *body;
    SOCKET socket_handle;
    unsigned short port;
    unsigned long response_length;
    unsigned long display_length;
    BOOL disable_thinking;

    GetDlgItemText(dialog, IDC_SERVER, g_server, sizeof(g_server));
    GetDlgItemText(dialog, IDC_PORT, g_port_text, sizeof(g_port_text));
    GetDlgItemText(dialog, IDC_MODEL, g_model, sizeof(g_model));
    GetDlgItemText(dialog, IDC_PROMPT, g_prompt, sizeof(g_prompt));
    port = (unsigned short)atoi(g_port_text);
    disable_thinking = (IsDlgButtonChecked(dialog, IDC_THINK) != 0);
    SetDlgItemText(dialog, IDC_RESPONSE, "");

    if (g_server[0] == '\0' || port == 0 || g_model[0] == '\0' || g_prompt[0] == '\0') {
        SetStatus(dialog, "Enter server, port, model, prompt.");
        return;
    }
    if (!BuildGenerateRequest(g_model, g_prompt, disable_thinking)) {
        SetStatus(dialog, "Prompt is too long for Win16.");
        return;
    }
    SetStatus(dialog, "Starting Winsock 1.1...");
    if (!StartWinsock(dialog)) {
        return;
    }
    socket_handle = ConnectToServer(dialog, g_server, port);
    if (socket_handle == INVALID_SOCKET) {
        WSACleanup();
        return;
    }
    SetStatus(dialog, "Sending request to Ollama...");
    if (!SendAll(socket_handle, g_request, HugeStrLen(g_request))) {
        SetSocketError(dialog, "send");
        closesocket(socket_handle);
        WSACleanup();
        return;
    }
    switch (ReceiveAll(socket_handle, &response_length)) {
    case RECEIVE_TOO_LARGE:
        SetStatus(dialog, "Response too large; shorten prompt.");
        closesocket(socket_handle);
        WSACleanup();
        return;
    case RECEIVE_SOCKET_ERROR:
        SetSocketError(dialog, "recv");
        closesocket(socket_handle);
        WSACleanup();
        return;
    default:
        break;
    }
    closesocket(socket_handle);
    WSACleanup();
    SetStatus(dialog, "HTTP response received...");
    if (response_length < 12 || !HugeStartsWith(g_response, "HTTP/1.") ||
        g_response[9] != '2') {
        SetStatus(dialog, "Ollama returned an HTTP error.");
        return;
    }
    body = FindResponseBody(g_response);
    if (body == NULL) {
        SetStatus(dialog, "Could not find response body.");
        return;
    }
    if (IsChunkedResponse(g_response)) {
        SetStatus(dialog, "Decoding chunked Ollama response...");
        /* Decoded in place: chunk framing is always consumed without
           being written out, so this never writes ahead of what has
           already been read (see DecodeChunkedBody). */
        if (!DecodeChunkedBody(body, body, RESPONSE_SIZE - (unsigned long)(body - g_response))) {
            SetStatus(dialog, "Could not decode chunked response.");
            return;
        }
    }
    SetStatus(dialog, "Parsing Ollama JSON response...");
    /*
     * Grab the context array first, while body is still the complete,
     * untruncated JSON -- ExtractJsonResponse below overwrites body in
     * place and null-terminates it at the end of the "response" value,
     * which would otherwise hide "context" (it comes later in the
     * object) from any further string search. If this model didn't
     * return one, or it doesn't fit, we drop any old one rather than
     * resend context from a different exchange (or a different model).
     */
    if (!ExtractContextArray(body, g_context, CONTEXT_SIZE)) {
        g_context[0] = '\0';
    }
    /* Unescaped in place too: JSON escapes only ever shrink (\n, \t,
       \u003c etc. all collapse to one output byte), and the "response"
       field starts well after this buffer's first byte, so the write
       cursor can never catch up to the read cursor. */
    if (!ExtractJsonResponse(body, body, RESPONSE_SIZE - (unsigned long)(body - g_response))) {
        SetStatus(dialog, "Could not read JSON response.");
        return;
    }
    SetStatus(dialog, "Displaying Ollama response...");
    display_length = HugeStrLen(body);
    if (display_length > SAFE_DISPLAY_LIMIT) {
        /* Win 3.1's edit control has its own practical text-length
           ceiling well under our new response capacity; show what fits
           rather than risk it choking on a very long string. */
        body[SAFE_DISPLAY_LIMIT] = '\0';
        SetDlgItemText(dialog, IDC_RESPONSE, (LPCSTR)body);
        SetStatus(dialog, "Response received (truncated).");
    } else {
        SetDlgItemText(dialog, IDC_RESPONSE, (LPCSTR)body);
        SetStatus(dialog, g_context[0] != '\0' ?
                  "Received. Send again to continue." :
                  "Response received.");
    }
}

static BOOL FAR PASCAL DialogProcedure(HWND dialog, UINT message,
                                       WPARAM w_param, LPARAM l_param)
{
    (void)l_param;

    switch (message) {
    case WM_INITDIALOG:
        SetDlgItemText(dialog, IDC_SERVER, "192.168.1.132");
        SetDlgItemInt(dialog, IDC_PORT, WINLLAMA_PORT_DEFAULT, FALSE);
        SetDlgItemText(dialog, IDC_MODEL, "retro_qwen");
        SetDlgItemText(dialog, IDC_PROMPT, "Hello from WinLlama.");
        /* 1 is BST_CHECKED's value; older Win16 headers may not define
           the BST_* constants, so use the literal to stay portable. */
        CheckDlgButton(dialog, IDC_THINK, 1);
        SetStatus(dialog, "Ready. Enter server and test.");
        return TRUE;

    case WM_COMMAND:
        switch (w_param) {
        case IDC_SEND:
            GenerateResponse(dialog);
            return TRUE;

        case IDC_CLEAR:
            ClearFields(dialog);
            return TRUE;

        case IDC_TEST:
            TestConnection(dialog);
            return TRUE;

        case IDCANCEL:
            DestroyWindow(dialog);
            return TRUE;
        }
        break;

    case WM_CLOSE:
        DestroyWindow(dialog);
        return TRUE;

    case WM_DESTROY:
        PostQuitMessage(0);
        return TRUE;
    }

    return FALSE;
}

int PASCAL WinMain(HINSTANCE instance, HINSTANCE previous_instance,
                   LPSTR command_line, int show_command)
{
    HWND dialog;
    MSG message;

    (void)previous_instance;
    (void)command_line;
    (void)show_command;
    g_instance = instance;

    if (!AllocateBigBuffers()) {
        MessageBox(NULL,
            "Not enough memory for WinLlama's buffers. Free up system "
            "memory, or ask for smaller buffer sizes, and try again.",
            "WinLlama", MB_OK | MB_ICONSTOP);
        FreeBigBuffers();
        return 1;
    }

    dialog = CreateDialog(g_instance, MAKEINTRESOURCE(WINLLAMA_DIALOG),
                          NULL, DialogProcedure);
    if (dialog == NULL) {
        MessageBox(NULL, "Unable to create the main dialog.", "WinLlama", MB_OK | MB_ICONSTOP);
        FreeBigBuffers();
        return 1;
    }

    ShowWindow(dialog, SW_SHOW);
    while (GetMessage(&message, NULL, 0, 0)) {
        if (!IsDialogMessage(dialog, &message)) {
            TranslateMessage(&message);
            DispatchMessage(&message);
        }
    }

    FreeBigBuffers();
    return message.wParam;
}
