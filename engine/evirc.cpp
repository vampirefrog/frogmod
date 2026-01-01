#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>

#include "evirc.h"
#include "cube.h"

namespace IRC {

static void irc_readcb(struct bufferevent *buf, void *arg) {
	Server *s = (Server *)arg;
	if(s->state < Server::Connected) return;
	s->read();
}

static void irc_writecb(struct bufferevent *buf, void *arg) {
	return;
}

static void irc_eventcb(struct bufferevent *buf, short what, void *arg) {
	Server *s = (Server *)arg;
	if(what == BEV_EVENT_CONNECTED) {
		s->state = Server::Connected;
		DEBUGF(bufferevent_enable(s->buf, EV_READ));
		struct evbuffer *eb;
		DEBUGF(eb = evbuffer_new());
		if(s->pass && s->pass[0]) DEBUGF(evbuffer_add_printf(eb, "PASS %s\r\n", s->pass));
		DEBUGF(evbuffer_add_printf(eb, "USER %s 0 * :%s\r\nNICK %s\r\n", s->nick, s->nick, s->nick));
		DEBUGF(bufferevent_write_buffer(buf, eb));
		DEBUGF(evbuffer_free(eb));
		s->state = Server::SentIdent;
	} else {
		if(what == (BEV_EVENT_EOF | BEV_EVENT_READING))
			printf("Disconnected from \"%s\".\n", s->host);
		else bufferevent_print_error(what, "Disconnected from \"%s\":", s->host);

		DEBUGF(bufferevent_free(s->buf));
		s->buf = NULL;

		if(s->state == Server::Quitting) {
			if(s->client->server_quit_cb) s->client->server_quit_cb(s);
			s->client->delserv(s);
		} else {
			s->state = Server::None; // disconnected
			timeval tv;
			tv.tv_sec = 10;
			tv.tv_usec = 0;
			printf("Reconnecting in %d seconds...\n", (int)tv.tv_sec);
			DEBUGF(evtimer_add(s->reconnect_event, &tv));
		}
	}
}

static void irc_connectcb(int fd, short type, void *arg) {
	Server *s = (Server *)arg;
	DEBUGF(s->buf = bufferevent_socket_new(s->client->base, -1, BEV_OPT_CLOSE_ON_FREE));
	DEBUGF(bufferevent_setcb(s->buf, irc_readcb, irc_writecb, irc_eventcb, s));
	bufferevent_socket_connect_hostname(s->buf, s->client->dnsbase, AF_UNSPEC, s->host, s->port);
}

void Server::init() {
	DEBUGF(evbuf = evbuffer_new());
	buf = NULL;
	DEBUGF(reconnect_event = evtimer_new(client->base, irc_connectcb, this));
}

bool Server::connect(const char *h, const char *n, const char *w, int p, const char *alias_) {
	host = strdup(h);
	port = p;
	pass = w&&w[0]?strdup(w):0;
	if(alias_) alias = strdup(alias_); else alias = strdup(h);
	nick = strdup(n);
	irc_connectcb(0, 0, this);
	return true;
}

void Server::quit(const char *msg, int quitsecs) {
	if(state >= Connected && state != Quitting) {
		if(msg) DEBUGF(bufferevent_write_printf(buf, "QUIT :%s\r\n", msg))
		else DEBUGF(bufferevent_write_printf(buf, "QUIT\r\n"))
		bufferevent_flush(buf, EV_WRITE, BEV_FLUSH);
		struct timeval readtv;
		readtv.tv_sec = quitsecs;
		readtv.tv_usec = 0;
		bufferevent_set_timeouts(buf, &readtv, NULL); // we don't want to wait for the server to close the connection more than a few seconds
		state = Quitting;
	}
}

void Server::join(const char *channel, int verbosity_, const char *alias_, const char *pwd_) {
	Channel *c = NULL;
	for(unsigned int i = 0; i < channels.size(); i++) {
		if(!strcmp(channels[i]->name, channel)) c = channels[i]; // don't join an existing channel
	}
	if(!c) {
		c = new Channel(this, channel, verbosity_, alias_, pwd_);
		channels.push_back(c);
	}
	if(state == Active)
		DEBUGF(bufferevent_write_printf(buf, "JOIN %s %s\r\n", c->name, c->pwd?c->pwd:""));
}

void Server::part(const char *channel) {
	for(unsigned int i = 0; i < channels.size(); i++) {
		if(!strcmp(channels[i]->name, channel)) {
			delete channels[i];
			channels.erase(channels.begin() + i);
			break;
		}
	}
	if(state == Active)
		DEBUGF(bufferevent_write_printf(buf, "PART %s\r\n", channel));
}

void Server::read() {
	DEBUGF(bufferevent_read_buffer(buf, evbuf));
	char *ln;
	while((ln = evbuffer_readln(evbuf, NULL, EVBUFFER_EOL_ANY))) {
		parse(ln);
		free(ln);
	}
	return;
}

void Server::process(char *prefix, char *command, char *params[], int nparams, char *trailing) {
	if(!strcmp(command, "PRIVMSG") && prefix) {
		Source s;
		s.server = this;
		s.client = client;
		s.peer = findpeer(stripident(prefix));
		int l = strlen(trailing);
		if(*trailing == 1 && trailing[l - 1] == 1) { // special privmsg
			trailing[l - 1] = 0;
			if(!strncmp(trailing + 1, "PING", 4)) {
				if(!is_chan(prefix)) {
					struct evbuffer *eb = evbuffer_new();
					evbuffer_add_printf(eb, "NOTICE %s :\001PING %s\001\r\n", prefix, trailing + 6);
					bufferevent_write_buffer(buf, eb);
					evbuffer_free(eb);
				}
			} else if(!strncmp(trailing + 1, "VERSION", 7)) {
				if(!is_chan(prefix) && client->version_cb) {
					s.channel = NULL;
					client->version_cb(&s);
				}
			} else if(!strncmp(trailing + 1, "ACTION", 6)) {
				if(is_chan(params[0])) {
					s.channel = findchan(params[0]);
					if(client->channel_action_message_cb) client->channel_action_message_cb(&s, trailing + 8);
				} else {
					s.channel = NULL;
					if(client->private_action_message_cb) client->private_action_message_cb(&s, trailing + 8);
				}
			}
		} else {
			if(is_chan(params[0])) {
				s.channel = findchan(params[0]);
				if(client->channel_message_cb) client->channel_message_cb(&s, trailing);
			} else {
				s.channel = NULL;
				if(client->private_message_cb) client->private_message_cb(&s, trailing);
			}
		}
	} else if(!strcmp(command, "NOTICE")) {
		if(client->notice_cb) client->notice_cb(this, prefix, trailing);
	} else if(!strcmp(command, "NICK")) {
		char *newnick = nparams>0?params[0]:trailing;
		if(client->nick_cb) {
			Source s;
			s.server = this;
			s.client = client;
			s.peer = findpeer(stripident(prefix));
			s.channel = NULL;
			client->nick_cb(&s, newnick);
		}
		changenick(prefix, newnick);
	} else if(!strcmp(command, "PART")) {
		if(nparams >= 1) {
			Channel *c = findchan(params[0]);
			if(c) {
				Source s;
				s.server = this;
				s.client = client;
				s.peer = findpeer(stripident(prefix));
				s.channel = c;
				c->peerpart(prefix);
				if(client->part_cb) client->part_cb(&s, trailing);
			}
		}
	} else if(!strcmp(command, "JOIN")) {
		Channel *c = findchan(trailing);
		if(!c && nparams >= 1) c = findchan(params[0]);
		if(c) {
			Source s;
			s.server = this;
			s.client = client;
			s.peer = findpeer(stripident(prefix));
			s.channel = c;
			c->peerjoin(stripident(prefix));
			if(client->join_cb) client->join_cb(&s);
		}
	} else if(!strcmp(command, "QUIT")) {
		for(unsigned int i = 0; i < peers.size(); i++) {
			if(!strcmp(peers[i]->nick, stripident(prefix))) {
				if(client->quit_cb) {
					Source s;
					s.server = this;
					s.client = client;
					s.peer = findpeer(stripident(prefix));
					s.channel = NULL; // quit is server-wide
					client->quit_cb(&s, trailing);
				}
				for(unsigned int j = 0; j < channels.size(); j++) {
					Channel *c = channels[j];
					for(unsigned int k = 0; k < c->peers.size(); k++) {
						if(c->peers[k]->peer == peers[i]) {
							c->peers.erase(c->peers.begin() + k);
							break;
						}
					}
				}
				delete peers[i];
				peers.erase(peers.begin() + i);
				break;
			}
		}
	} else if(!strcmp(command, "PING")) {
		if(trailing) DEBUGF(bufferevent_write_printf(buf, "PONG %s\r\n", trailing))
		else DEBUGF(bufferevent_write_printf(buf, "PONG\r\n"));
		if(client->ping_cb) client->ping_cb(this, NULL, trailing);
	} else if(!strcmp(command, "MODE")) {
		if(client->mode_cb) {
			Source s;
			s.server = this;
			s.client = client;
			s.peer = findpeer(stripident(prefix));
			s.channel = nparams>=1?findchan(params[0]):NULL;
			client->mode_cb(&s, stripident(prefix), nparams>=2?params[1]:NULL, nparams>=3?params[2]:NULL);
		}
	} else if(!strcmp(command, "TOPIC")) {
		if(client->topic_cb && strcmp(stripident(prefix), nick)) {
			Source s;
			s.server = this;
			s.client = client;
			s.peer = findpeer(stripident(prefix));
			s.channel = findchan(params[0]);
			if(s.channel) client->topic_cb(&s, trailing);
		}
	} else if(isdigit(*command)) {
		int numeric = atoi(command);
		switch(numeric) {
			case 001:
				if(state != Active) {
					state = Active;
					for(unsigned int i = 0; i < channels.size(); i++) {
						join(channels[i]->name);
					}
				}
				break;
			case 372:
			case 376:
				if(client->motd_cb) client->motd_cb(this, NULL, trailing);
				break;
			case 353:
				if(nparams >= 3) {
					Channel *c = findchan(params[2]);
					if(c) {
						char *nick = strtok(trailing, " ");
						while(nick) {
							c->peerjoin(nick);
							nick = strtok(NULL, " ");
						}
					}
				}
				break;
			case 433:
			{ //FIXME: add a callback for this
				struct evbuffer *eb;
				DEBUGF(eb = evbuffer_new());
				int nl = strlen(nick);
				char *newnick = (char *)malloc(nl + 2); //FIXME: this sux
				sprintf(newnick, "%s_", nick);
				printf("Changing nick from %s to %s\n", nick, newnick);
				free(nick);
				nick = newnick;
				DEBUGF(evbuffer_add_printf(eb, "USER %s 0 * :%s\r\nNICK %s\r\n", nick, nick, nick));
				DEBUGF(bufferevent_write_buffer(buf, eb));
				DEBUGF(evbuffer_free(eb));
				break;
			}
		}
	} else {
		if(client->unhandled_cb) client->unhandled_cb(this, prefix, command, params, nparams, trailing);
	}
}

void Server::parse(char *line) {
	char *c = line;
	char *prefix = NULL;

	// parse prefix, if any
	if(*c == ':') {
		while(*c && *c != ' ') c++;
		if(*c) { *c++ = 0; }
		prefix = line+1;
	}

#define NPARAMS 30
	char *params[NPARAMS]; int i = 0;
	while(*c) {
		while(*c && *c == ' ') c++;
		if(!*c || *c == ':') break;
		if(i < NPARAMS) params[i++] = c;
		while(*c && *c != ' ') c++;
		if(*c) { *c++ = 0; }
	}

	char *trailing = NULL;
	if(*c == ':') trailing = c+1;

	if(i > 0) process(prefix, params[0], params + 1, i - 1, trailing);
}

void Server::raw(const char  *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	this->vraw(fmt, ap);
	va_end(ap);
}

void Server::vraw(const char *fmt, va_list ap) {
	if(state != Active) return;

	evbuffer *evb = evbuffer_new();
	evbuffer_add_vprintf(evb, fmt, ap);
	bufferevent_write_buffer(buf, evb);
	evbuffer_free(evb);
}

void Server::speak(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vspeak(fmt, ap);
	va_end(ap);
}

void Server::vspeak(const char *fmt, va_list ap) {
	for(unsigned int i = 0; i < channels.size(); i++) {
		channels[i]->vspeak(fmt, ap);
	}
}

void Server::speak(int verbosity_, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vspeak(verbosity_, fmt, ap);
	va_end(ap);
}

void Server::vspeak(int verbosity_, const char *fmt, va_list ap) {
	for(unsigned int i = 0; i < channels.size(); i++) {
		channels[i]->vspeak(verbosity_, fmt, ap);
	}
}

void Server::speakto(const char *who, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vspeakto(who, fmt, ap);
	va_end(ap);
}

void Server::vspeakto(const char *to, const char *fmt, va_list ap) {
	evbuffer *evb;
	DEBUGF(evb = evbuffer_new());
	DEBUGF(evbuffer_add_vprintf(evb, fmt, ap));
	DEBUGF(evbuffer_add_printf(evb, "\r\n")); // force last line... evbuffer_readln...
	bspeakto(to, evb);
	DEBUGF(evbuffer_free(evb));
}

void Server::bspeakto(const char *to, evbuffer *evb) {
	if(state != Active) return;

	char *l;
	while((l = evbuffer_readln_nul(evb, NULL, EVBUFFER_EOL_ANY))) { // split string into separate lines
		if(strlen(l) < 1) continue;
		evbuffer *msgb;
		DEBUGF(msgb = evbuffer_new());
		DEBUGF(evbuffer_add_printf(msgb, "PRIVMSG %s :%s\r\n", to, l));
		DEBUGF(bufferevent_write_buffer(buf, msgb));
		DEBUGF(evbuffer_free(msgb));
		free(l);
	}
}

void Server::changenick(char *old, char *nick) {
	Peer *p = findpeer(stripident(old));
	if(p) p->set_nick(nick);
}

Peer *Server::addpeer(char *nick) {
	stripident(nick);
	for(unsigned int i = 0; i < peers.size(); i++) {
		if(!strcmp(peers[i]->nick, nick)) return peers[i];
	}
	Peer *p = new Peer;
	if(p) {
		p->set_nick(nick);
		peers.push_back(p);
		return p;
	}

	return NULL;
}

void Server::delpeer(Peer *p) {
	for(unsigned int i = 0; i < peers.size(); i++) {
		if(peers[i] == p) { peers.erase(peers.begin() + i); return; }
	}
}

Channel::Channel(Server *serv, const char *channel, int verbosity_, const char *alias_, const char *pwd_) {
	name = strdup(channel);
	if(alias_) alias = strdup(alias_);
	else {
		char buf[512];
		snprintf(buf, 512, "%s %s", serv->alias, channel);
		alias = strdup(buf);
	}
	server = serv;
	verbosity = verbosity_;
	pwd = (pwd_&&*pwd_)?strdup(pwd_):NULL;
}

Channel::~Channel() {
	if(name) free(name);
	if(alias) free(alias);
	if(pwd) free(pwd);
}

ChannelPeer *Channel::peerjoin(char *nick) {
	while(*nick && (*nick == '@' || *nick == '+')) nick++; // strip privileges. FIXME: get prefixes from server info string
	Peer *p = server->addpeer(nick);
	if(p) {
		for(unsigned int i = 0; i < peers.size(); i++) {
			if(peers[i]->peer == p) return peers[i];
		}
		ChannelPeer *cp = new ChannelPeer;
		if(cp) {
			p->count++;
			cp->peer = p;
			peers.push_back(cp);
			return cp;
		}
	}
	return NULL;
}

void Channel::peerpart(char *nick) {
	stripident(nick);

	for(unsigned int i = 0; i < peers.size(); i++) {
		ChannelPeer *p = peers[i];
		if(!strcmp(p->peer->nick, nick)) {
			p->peer->count--;
			if(p->peer->count <= 0) {
				server->delpeer(p->peer);
			}
			delete p;
			peers.erase(peers.begin() + i);
			return;
		}
	}
}

void Channel::speak(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vspeak(fmt, ap);
	va_end(ap);
}

void Channel::vspeak(const char *fmt, va_list ap) {
	server->vspeakto(name, fmt, ap);
}

void Channel::speak(int verbosity_, const char *fmt, ...) {
	if(verbosity_ > verbosity) return; // early fail

	va_list ap;
	va_start(ap, fmt);
	vspeak(verbosity_, fmt, ap);
	va_end(ap);
}

void Channel::vspeak(int verbosity_, const char *fmt, va_list ap) {
	if(verbosity_ > verbosity) return;

	vspeak(fmt, ap);
}

void Source::reply(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vreply(fmt, ap);
	va_end(ap);
}

void Source::vreply(const char *fmt, va_list ap) {
	if(channel) {
		evbuffer *evb;
		DEBUGF(evb = evbuffer_new());
		DEBUGF(evbuffer_add_printf(evb, "%s: ", peer->nick));
		DEBUGF(evbuffer_add_vprintf(evb, fmt, ap));
		server->bspeakto(channel->name, evb);
		DEBUGF(evbuffer_free(evb));
	} else {
		server->vspeakto(peer->nick, fmt, ap);
	}
}

void Source::speak(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vspeak(fmt, ap);
	va_end(ap);
}

void Source::vspeak(const char *fmt, va_list ap) {
	if(channel) {
		evbuffer *evb;
		DEBUGF(evb = evbuffer_new());
		DEBUGF(evbuffer_add_vprintf(evb, fmt, ap));
		server->bspeakto(channel->name, evb);
		DEBUGF(evbuffer_free(evb));
	} else {
		server->vspeakto(peer->nick, fmt, ap);
	}
}

bool Client::connect(const char *host, const char *nick, const char *pass, int port, const char *alias) {
	Server *s = new Server;
	s->client = this;
	s->init();
	s->connect(host, nick, pass, port, alias);
	servers.push_back(s);
	return true;
}

void Client::quit(const char *msg, int quitsecs) {
	for(unsigned int i = 0; i < servers.size(); i++) {
		servers[i]->quit(msg, quitsecs);
	}
}

void Client::speak(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vspeak(fmt, ap);
	va_end(ap);
}

void Client::vspeak(const char *fmt, va_list ap) {
	for(unsigned int i = 0; i < servers.size(); i++) {
		servers[i]->vspeak(fmt, ap);
	}
}

void Client::speak(int verbosity_, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vspeak(verbosity_, fmt, ap);
	va_end(ap);
}

void Client::vspeak(int verbosity_, const char *fmt, va_list ap) {
	for(unsigned int i = 0; i < servers.size(); i++) {
		servers[i]->vspeak(verbosity_, fmt, ap);
	}
}

char *stripident(char *ident) {
	if(!ident) return NULL;
	char *end = strpbrk(ident, "!@");
	if(end) *end = 0;
	return ident;
}

} // namespace IRC
