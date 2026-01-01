struct netbuf {
	char *data;
	int size, pos;

	netbuf(char *init) {
		int l = strlen(init);
		size = l;
		pos = l;
		data = new char[size+1];
		strcpy(data, init);
	}

	netbuf(int startsize = 1024) {
		size = startsize;
		pos = 0;
		data = new char[size+1];
		data[0] = 0;
	}

	~netbuf() {
		DELETEP(data);
	}

	int put(char c) {
		if(pos == size) {
			size += 1024;
			char *newdata = new char[size+1];
			if(data) {
				strncpy(newdata, data, size);
				delete data;
			}
			data = newdata;
		}
		data[pos++] = c;
		data[pos] = 0;
		return pos;
	}

	int put(const char *s, int l = -1) {
		if(l < 0) l = strlen(s);
		if(pos + l >= size) {
			size = pos + l;
			char *newdata = new char[size+1];
			strncpy(newdata, data, pos);
			if(data) delete data;
			data = newdata;
		}
		strncpy(data+pos, s, l);
		pos += l;
		return pos;
	}

	int put(netbuf &b) {
		return put(b.data, b.pos);
	}

	void putf(const char *fmt, ...) {
		defvformatstring(str, fmt, fmt);
		put(str);
	}

	void clear() {
		DELETEP(data);
		size = 0;
		pos = 0;
	}

	inline int length() { return pos; }

	inline operator char *() { return data; }
};
