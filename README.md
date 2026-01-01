frogmod-collect
===============

A server modification for Sauerbraten Collect edition

Installation guide
------------------

First, make sure you have make and g++ installed. In Debian you can do apt-get install build-essential.  
Now compile by running `make`.  
Next, copy `server-init.cfg.example` to `server-init.cfg` and edit the default values.  
You should edit at least `serverdesc`, `servermotd` and `adminpass`.  
If you want to use the IRC bot (see below), you should enable rcon by setting `rconport` to something like 28888, and for security, make sure `rconip` is `localhost` or `127.0.0.1`.  
Run the server by typing `./sauer_server`.  

If you want an IRC bot, you should run the node script `frogserv.js`, thusly:  
First, install [node.js](http://nodejs.org/).  
Go to the `node` directory and copy config.json.example to config.json, and edit the values in there.  
Make sure the rcon port is the same as the `rconport` variable you set earlier.  
You'll also need to install some node modules by running `npm install irc` in the `node` folder.  
Then, run the node script by doing `node frogserv.js` in the `node` folder.  
