# Serve the dashboard with nginx

The hub's data directory is a static website. This guide serves it with nginx, over HTTPS and behind a password.

The built-in `--http` server has no TLS and no login. Use it only on `127.0.0.1` or a trusted network.

## Before you start

- The hub runs on the same machine as nginx.
- A domain name, such as `quectop.example.com`, points to the machine.
- The nginx user can read the hub's data directory. This guide uses `/var/lib/quectop-hub/site`.
- The keys file is outside the data directory. nginx serves every file in that directory.

## Create a password file

1. Create a user name and password for the dashboard:

	```sh
	printf 'me:%s\n' "$(openssl passwd -apr1)" | sudo tee /etc/nginx/quectop.htpasswd
	```

2. Let only the nginx user read the file:

	```sh
	sudo chown root:www-data /etc/nginx/quectop.htpasswd
	sudo chmod 640 /etc/nginx/quectop.htpasswd
	```

	On Debian and Ubuntu, nginx runs as `www-data`. On other systems, use the user from the `user` line in `nginx.conf`.

## Add the site

1. Create `/etc/nginx/sites-available/quectop` with this content:

	```nginx
	server {
		listen 80;
		server_name quectop.example.com;
		root /var/lib/quectop-hub/site;

		auth_basic "quectop";
		auth_basic_user_file /etc/nginx/quectop.htpasswd;

		add_header Cache-Control no-cache;
		location ~ /\. { deny all; }
	}
	```

	The `location` block hides temporary files and the revision marker, whose names start with a dot. `no-cache` makes the dashboard check for new data on every poll.

2. Enable the site and reload nginx:

	```sh
	sudo ln -s /etc/nginx/sites-available/quectop /etc/nginx/sites-enabled/
	sudo nginx -t && sudo systemctl reload nginx
	```

## Turn on HTTPS

Run certbot to get a certificate:

```sh
sudo certbot --nginx -d quectop.example.com
```

Certbot adds `listen 443 ssl` and the certificate lines to the site. It also redirects HTTP to HTTPS. Without HTTPS, the browser sends your password in a form that anyone on the network can decode.

## Check the site

Open `https://quectop.example.com/`. The browser asks for the user name and password, then shows the dashboard.

| Problem | Fix |
| --- | --- |
| `403 Forbidden` | Let the nginx user read `/var/lib/quectop-hub` and every directory above it. |
| The page loads but shows no host | Check that `hosts.txt` exists in the data directory. The hub creates it when it starts. |
| The data stops updating | Check that the hub is running and that the device is connected. See [Monitor another machine](monitor-another-machine.md#check-the-connection). |
