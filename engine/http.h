/* ENet-based http functionality */
#ifndef HTTP_H_
#define HTTP_H_

#include "netbuf.h"

struct httpheader {
	char *key, *value;
};

struct httprequest;
struct httpcb {
	void (*cb)(httprequest *, void *data);
	void *data;
};

enum httprequeststate {
	START = 0, // before method
	IN_METHOD, // GET, POST etc.
	AFTER_METHOD,
	IN_PATH, // /path/to/file.html?foo=bar&baz=quux
	AFTER_PATH,
	IN_PROTOCOL, // HTTP/1.0
	AFTER_PROTOCOL,
	AFTER_PROTOCOL_LINE, // next, we have either headers or the content begins
	IN_HEADER_NAME,
	BEFORE_HEADER_VALUE,
	IN_HEADER_VALUE,
	AFTER_HEADER_VALUE,
	IN_BODY,
	IN_CHUNK_LENGTH,
	IN_CHUNK,
	AFTER_CHUNK,
	AFTER_REQUEST,
	AFTER_RESPONSE
};

enum {
	HTTPREQ_NOT_IN_SOCKSET,
	HTTPREQ_DONE,
	HTTPREQ_THANKS,
	HTTPREQ_DISCONNECTED
};

struct httpserver;
struct httprequest {
	ENetSocket sock;
	ENetAddress addr;
	httpserver *server;

	// Request state
	netbuf method, path, protocol;
	netbuf req_content;
	vector<httpheader> req_headers;
	netbuf header_key, header_value;
	httprequeststate state;
	int content_length;
	// chunked encoding
	bool chunked;
	int chunk_length, chunk_pos;
	netbuf chunk_length_str;
	bool keep_alive; // Connection: Keep-Alive requested

	// Response stuff
	vector<httpheader> headers;
	netbuf content;

	httprequest(httpserver *server_, ENetSocket sock_, ENetAddress addr_) {
		init(server_, sock_, addr_);
	}

	void init(httpserver *server_, ENetSocket sock_, ENetAddress addr_) {
		server = server_;
		sock = sock_;
		addr = addr_;
		state = START;
		content_length = 0;
		chunked = false;
		keep_alive = true;
	}

	void reset() {
		content_length = 0;
		chunked = false;
		keep_alive = true; // default for HTTP/1.1
		method.clear();
		path.clear();
		protocol.clear();
		req_content.clear();
		loopv(req_headers) {
			if(req_headers[i].key) delete req_headers[i].key;
			if(req_headers[i].value) delete req_headers[i].value;
		}
		req_headers.shrink(0);
		header_key.clear();
		header_value.clear();
		content.clear();
		loopv(headers) {
			if(headers[i].key) delete headers[i].key;
			if(headers[i].value) delete headers[i].value;
		}
		headers.shrink(0);
		state = START;
	}

	~httprequest() {
		reset();
 	}

	void add_req_header(const char *key, const char *value) {
		httpheader &h = req_headers.add();
		h.key = key?newstring(key):NULL;
		h.value = value?newstring(value):NULL;
		if(key && value) {
			if(!strcasecmp(key, "content-length")) content_length = atoi(value);
			if(!strcasecmp(key, "transfer-encoding") && !strcasecmp(value, "chunked")) chunked = true;
			if(!strcasecmp(key, "connection") && !strcasecmp(value, "close")) keep_alive = false;
		}
	}

	int check_sockset(ENetSocketSet &sockset) {
		if(!ENET_SOCKETSET_CHECK(sockset, sock)) return HTTPREQ_NOT_IN_SOCKSET;
		char data[4096];
		ENetBuffer buf;
		buf.data = data;
		buf.dataLength = sizeof(data);
		while(1) {
			int r = enet_socket_receive(sock, NULL, &buf, 1);
			if(r > 0) {
				// process input
				int left = 0;
				while(left < r) {
					left = parse(data, left, r);
					if(state == AFTER_REQUEST) {
						return HTTPREQ_DONE;
					}
				}
			} else { // connection lost
				// TODO: clean up connection?
				enet_socket_destroy(sock);
				return HTTPREQ_DISCONNECTED;
			}
			if((unsigned int)r < sizeof(data)) break;
		}

		return HTTPREQ_THANKS;
	}

	int parse(const char *data, int start, int end) {
		int i;
		for(i = start; i < end; i++) {
			const char c = data[i];
			if(state < IN_BODY && c == '\r') {
				continue; // ignore carriage returns in header section
			}
			switch(state) {
				case START:
					if(isalpha(c)) {
						state = IN_METHOD;
						method.put(c);
					} else if(c == '\n') {
						state = AFTER_PROTOCOL_LINE;
					} else { // ignore characters before method
						printf("HTTP: Warning: extra character before method [%c] (%d)\n", c, c);
					}
					break;
				case IN_METHOD:
					if(isalpha(c)) {
						method.put(c);
					} else if(c == '\n') {
						state = AFTER_PROTOCOL_LINE;
					} else if(isspace(c)) {
						state = AFTER_METHOD;
					} else { // ignore garbage characters
						printf("HTTP: Warning: garbage character in method [%c] (%d)\n", c, c);
					}
					break;
				case AFTER_METHOD:
					if(c == '\n') { // premature end but oh well
						state = AFTER_PROTOCOL_LINE;
					} else if(isspace(c)) { // ignore spaces up to path
					} else {
						state = IN_PATH;
						path.put(c);
					}
					break;
				case IN_PATH:
					if(c == '\n') { // premature end of line
						printf("HTTP: Warning: premature end of line when parsing path\n");
						state = AFTER_PROTOCOL_LINE;
					} else if(isspace(c)) {
						state = AFTER_PATH;
					}
					break;
				case AFTER_PATH:
					if(c == '\n') { // premature again
						printf("HTTP: Warning: premature end of line after parsing path\n");
						state = AFTER_PROTOCOL_LINE;
					} else if(isspace(c)) { // ignore spaces
					} else {
						state = IN_PROTOCOL;
						protocol.put(c);
					}
					break;
				case IN_PROTOCOL:
					if(c == '\n') {
						state = AFTER_PROTOCOL_LINE;
					} else if(isspace(c)) {
						state = AFTER_PROTOCOL;
					} else {
						protocol.put(c);
					}
					break;
				case AFTER_PROTOCOL: // just spaces
					if(c == '\n') {
						state = AFTER_PROTOCOL_LINE;
					} else {
						printf("HTTP: Warning: extra character after protocol line [%c] (%d)\n", c, c);
					}
					break;
				case AFTER_PROTOCOL_LINE:
					if(c == '\n') { // no headers
						//FIXME: respond with error
						state = IN_CHUNK_LENGTH; // content-encoding is assumed to be chunked, since we have no content-length...
					} else {
						state = IN_HEADER_NAME;
						header_key.put(c);
					}
					break;
				case IN_HEADER_NAME:
					if(c == '\n') {
						state = AFTER_HEADER_VALUE; // header key without value
						add_req_header(header_key, NULL);
						header_key.clear();
					} else if(c == ':') {
						state = BEFORE_HEADER_VALUE;
					} else header_key.put(c);
					break;
				case BEFORE_HEADER_VALUE:
					if(c == '\n') { // premature end
						state = AFTER_HEADER_VALUE;
						add_req_header(header_key, NULL);
						header_key.clear();
					} else if(!isspace(c)) {
						state = IN_HEADER_VALUE;
						header_value.put(c);
					}
					break;
				case IN_HEADER_VALUE:
					if(c == '\n') {
						add_req_header(header_key, header_value);
						header_key.clear();
						header_value.clear();
						state = AFTER_HEADER_VALUE;
					} else header_value.put(c);
					break;
				case AFTER_HEADER_VALUE:
					if(c == '\n') { // second newline
						if(chunked) state = IN_CHUNK_LENGTH;
						else if(content_length == 0) state = AFTER_REQUEST;
						else state = IN_BODY;
					} else {
						state = IN_HEADER_NAME;
						header_key.put(c);
					}
					break;
				case IN_BODY:
					req_content.put(c);
					if(req_content.length() >= content_length) {
						state = AFTER_REQUEST;
					}
					break;
				case IN_CHUNK_LENGTH:
					if(c == '\n') {
						chunk_length = strtol((char *)chunk_length_str, NULL, 16);
						chunk_length_str.clear();
						chunk_pos = 0;
						state = IN_CHUNK;
					} else if(isxdigit(c)) {
						chunk_length_str.put(c);
					}
					break;
				case IN_CHUNK:
					if(chunk_pos >= chunk_length) {
						state = AFTER_CHUNK;
					} else {
						req_content.put(c);
						chunk_pos++;
					}
					break;
				case AFTER_CHUNK:
					if(c == '\n') {
						if(chunk_length == 0) {
							state = AFTER_REQUEST;
						}
						else state = IN_CHUNK_LENGTH;
					}
				case AFTER_REQUEST: // wat?
					break;
				case AFTER_RESPONSE: // never reached
					break;
			}
		}
		return i;
	}

	/**
	 * Add a response header.
	 */
	void add_header(const char *key, const char *value) {
		httpheader &h = headers.add();
		h.key = key?newstring(key):NULL;
		h.value = value?newstring(value):NULL;
	}

	/**
	 * Add a response header.
	 */
	void add_header(const char *key, int value) {
		httpheader &h = headers.add();
		h.key = key?newstring(key):NULL;
		defformatstring(str, "%d", value);
		h.value = newstring(str);
	}

	void end(int code=200, const char *msg = "OK") {
		add_header("Content-type", "text/html");
		add_header("Content-length", content.length());
		add_header("Connection", keep_alive ? "Keep-Alive" : "Close");
		netbuf r;
		r.putf("HTTP/1.0 %d %s\r\n", code, msg);
		loopv(headers) {
			r.putf("%s: %s\r\n", headers[i].key, headers[i].value);
		}
		r.put("\r\n");
		r.put(content);
		ENetBuffer buf;
		buf.data = r.data;
		buf.dataLength = r.length();
		enet_socket_send(sock, NULL, &buf, 1);
		reset();
	}
};

struct httpserver {
	ENetSocket sock; // listening socket
	ENetAddress addr; // bind IP and port
	hashtable<const char *, httpcb> callbacks;
	void (*conn_lost_cb)(httprequest *);

	vector<httprequest *> reqs;

	httpserver(ENetAddress addr_) {
		sock = enet_socket_create(ENET_SOCKET_TYPE_STREAM);
		addr = addr_;
		if(sock != ENET_SOCKET_NULL && (enet_socket_set_option(sock, ENET_SOCKOPT_REUSEADDR, 1) < 0 || enet_socket_bind(sock, &addr) < 0)) {
			enet_socket_destroy(sock);
			conoutf("Could not bind http socket");
		}
		if(sock == ENET_SOCKET_NULL) {
			conoutf("Could not create http socket");
		} else {
			enet_socket_set_option(sock, ENET_SOCKOPT_NONBLOCK, 1);
			enet_socket_listen(sock, 5);
		}
	}

	~httpserver() {
	}

	void set_cb(const char *path, void(*cb_)(httprequest *, void *data), void *data = NULL) {
		httpcb &cb = callbacks[path];
		cb.cb = cb_;
		cb.data = data;
	}

	/** Adds the http server's socket to the given socket set. Returns the biggest socket descriptor.
	 */
	ENetSocket add_to_sockset(ENetSocketSet &sockset) {
		ENetSocket maxsock = sock;
		ENET_SOCKETSET_ADD(sockset, sock);
		loopv(reqs) {
			maxsock = max(maxsock, reqs[i]->sock);
			ENET_SOCKETSET_ADD(sockset, reqs[i]->sock);
		}
		return maxsock;
	}

	/** Checks the given socket set for own sockets (listening socket and connections' sockets)
	 */
	void check_sockset(ENetSocketSet &sockset) {
		if(ENET_SOCKETSET_CHECK(sockset, sock)) { // read data on listening socket means we get an incoming connection
			ENetAddress addr;
			ENetSocket connsock = enet_socket_accept(sock, &addr);
			if(connsock != ENET_SOCKET_NULL) {
				string str;
				enet_address_get_host_ip(&addr, str, sizeof(str));
				printf("HTTP: connection from %s\n", str);
				enet_socket_set_option(connsock, ENET_SOCKOPT_NONBLOCK, 1);
				reqs.add(new httprequest(this, connsock, addr));
			}
		}
		loopv(reqs) {
			string ipstr;
			enet_address_get_host_ip(&reqs[i]->addr, ipstr, sizeof(ipstr));
			switch(reqs[i]->check_sockset(sockset)) {
				case HTTPREQ_DONE:
				{
					httpcb *cb = callbacks.access((char *)reqs[i]->path);
					if(cb) {
						printf("HTTP: Request: %s\n", ipstr);
						cb->cb(reqs[i], cb->data);
					}
					break;
				}
				case HTTPREQ_DISCONNECTED:
				{
					printf("HTTP: Disconnected: %s\n", ipstr);
					if(conn_lost_cb) conn_lost_cb(reqs[i]);
					delete reqs[i];
					reqs.remove(i--);
					break;
				}
				case HTTPREQ_NOT_IN_SOCKSET:
					//printf("HTTP: Not in sockset\n");
					break;
				case HTTPREQ_THANKS:
					printf("HTTP: Thanks\n");
					break;
				default:
					printf("HTTP: ?\n");
					break;
			}
		}
	}
};

#endif /* HTTP_H_ */
