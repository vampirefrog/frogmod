var http = require('http');
var irc = require('irc');
var fs = require('fs');
if(!fs.existsSync('config.json')) {
	console.log('config.json not found! Please create config.json, using config.json.example as a guide, then re-run this script');
	process.exit(1);
}
var config = JSON.parse(fs.readFileSync('config.json'));

var irc = require('irc');
var ircclient = new irc.Client(config.irc.network, config.irc.nick, {
    channels: config.irc.channels,
});
ircclient.on('error', function(err) {
	console.log('irc error '+err);
});
function filtercubestr(s) {
	return s.replace(/(["^])/g, '^$1');
}
ircclient.addListener('message', function(from, to, message) {
	var cmd = 'gamesay "'+to+' \f1<' + from + '> \f0' + filtercubestr(message) + '"';
	var req = http.request({
			host: config.rcon.host,
			port: config.rcon.port,
			path: '/',
			method: 'POST'
		}, function(res) {
			// ignore response
		}
	);
	req.write(cmd);
	req.end();
	console.log(to, '<' + from + '>', message);
});

function froglog() {
	var req = http.request({
			host: config.rcon.host,
			port: config.rcon.port,
			path: '/',
			method: 'GET'
		}, function(res) {
			res.setEncoding('utf8');
			var body = '';
			res.on('data', function (chunk) {
				body += chunk;
			});
			res.on('end', function() {
				console.log(body);
				for(c in config.irc.channels)
					ircclient.say(config.irc.channels[c], body);
				froglog();
			});
		}
	);

	req.on('error', function(e) {
		console.log('Problem with request: ', e.message, '. Reconnecting in 5s');
		setTimeout(froglog, 5000);
	});

	req.end();
}

froglog();

http.createServer(function (req, res) {
  res.writeHead(200, {'Content-Type': 'text/plain'});
  res.end('Hello World\n');
}).listen(config.web.port, config.web.ip||undefined);
