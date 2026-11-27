# Publish a release

You need Docker Buildx, uv, and Git. Publishing also needs the release bucket credentials, a GPG signing key for APT and RPM, and SSH access to the AUR.

1. Update `VERSION` and add an entry to [the changelog](../../CHANGELOG.md). Record features and fixes. Commit the changes and push the matching tag with a `v` prefix, such as `v0.1.0`. The AUR stable source recipes build from that tag.
2. Build from the tagged checkout. Use a Buildx builder that supports `linux/amd64` and `linux/arm64`.

	```sh
	git submodule update --init
	make release-packages
	(cd dist/packages && sha256sum -c SHA256SUMS)
	```

3. Build the local APT and RPM repositories with `--dry-run`.

	```sh
	./yesb/release_apt.py --dry-run
	./yesb/release_apt.py --dry-run --project-root dist/packages/hub
	./yesb/release_rpm.py --dry-run
	./yesb/release_rpm.py --dry-run --project-root dist/packages/hub
	```

4. Publish the signed APT and RPM repositories to the shared packages host. Publish `quectop` before `quectop-hub` and run the publishers in sequence so each reads the current indexes. Pass a local credentials file using `--env PATH`. Set `AWS_ACCESS_KEY_ID`, `AWS_SECRET_ACCESS_KEY`, `AWS_ENDPOINT_URL`, `AWS_BUCKET`, and `GPG_KEY_ID` in that file.

	```sh
	./yesb/release_apt.py --env PATH
	./yesb/release_apt.py --env PATH --project-root dist/packages/hub
	./yesb/release_rpm.py --env PATH
	./yesb/release_rpm.py --env PATH --project-root dist/packages/hub
	```

5. For the first AUR release, publish all three variants of each package. Use an AUR account with SSH access. Yesb uploads the archives for the `-bin` recipes to the same location as the direct downloads.

	```sh
	./yesb/release_aur.py --env PATH --type all
	./yesb/release_aur.py --env PATH --type all --project-root dist/packages/hub
	```

	For later releases, run the AUR commands with `--type source` and `--type bin`. Refresh the `-git` recipe with `--type git` when its packaging changes. Its users build the current `main` when they rebuild.

6. Publish all four download archives and their checksum sidecars from the same build.

	```sh
	./yesb/release_direct.py --env PATH
	```

## Package names

| AUR package | What it installs |
| --- | --- |
| `quectop`, `quectop-hub` | Builds the tagged release from source |
| `quectop-bin`, `quectop-hub-bin` | Installs the prebuilt release |
| `quectop-git`, `quectop-hub-git` | Builds the current `main` branch |

For APT and RPM, install `quectop` on machines you want to monitor. Install `quectop-hub` on the dashboard machine. The package manager installs `quectop` as a dependency. On the AUR, any probe variant satisfies the hub dependency. For example, `yay -S quectop-bin quectop-hub-bin` installs both prebuilts.

For direct downloads, use the `quectop_VERSION_ARCH.tar.gz` archive on probe machines. Download both archives on a machine that runs both programs. Extract the archives and install the binaries from `usr/bin` in the extracted directory. See [Run quectop at boot](run-at-boot.md) to configure the services.
