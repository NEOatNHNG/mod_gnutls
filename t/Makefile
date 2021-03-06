#!/usr/bin/make -f

# Author: Daniel Kahn Gillmor <dkg@fifthhorseman.net>

# run these tests to ensure that mod_gnutls can handle a range of
# simple configuration choices.

export TEST_HOST ?= localhost
export TEST_IP ?= ::1
# chosen at random:
export TEST_PORT ?= 9932
export MSVA_PORT ?= 9933

export TEST_GAP ?= 1.5
export TEST_QUERY_DELAY ?= 2

all: setup.done
	./runtests

t-%: setup.done
	./runtests $@





### for setting up a little miniature CA + server + client environment:
identities := server authority client imposter rogueca
tokens := x509.pem secring.gpg secret.key cert.pgp secret.pgp
all_tokens := $(foreach id,$(identities),$(foreach token,$(tokens),$(id)/$(token)))

%.template: %.template.in
	sed s/__HOSTNAME__/$(TEST_HOST)/ < $< > $@

server.uid: server.uid.in
	sed s/__HOSTNAME__/$(TEST_HOST)/ < $< > $@

%/secret.key:
	mkdir -p $(dir $@)
	chmod 0700 $(dir $@)
	certtool --generate-privkey > $@

%/secring.gpg: %.uid %/secret.key
	rm -f $(dir $@)pubring.gpg $(dir $@)secring.gpg $(dir $@)trustdb.gpg
	PEM2OPENPGP_EXPIRATION=86400 PEM2OPENPGP_USAGE_FLAGS=authenticate,certify,sign pem2openpgp "$$(cat $<)" < $(dir $@)secret.key | GNUPGHOME=$(dir $@) gpg --import
	printf "%s:6:\n" "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" | GNUPGHOME=$(dir $@) gpg --import-ownertrust

%/gpg.conf: %/secring.gpg
	printf "default-key %s\n" "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" > $@

%/secret.pgp: %/secring.gpg
	GNUPGHOME=$(dir $@) gpg --armor --batch --no-tty --yes --export-secret-key "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" > $@

%/minimal.pgp: %/secring.gpg
	GNUPGHOME=$(dir $@) gpg --armor --export "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" > $@

%/cert.pgp: %/minimal.pgp authority/gpg.conf
	GNUPGHOME=authority gpg --import $<
	GNUPGHOME=authority gpg --batch --sign-key --no-tty --yes "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)"
	GNUPGHOME=authority gpg --armor --export "$$(GNUPGHOME=$(dir $@) gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" > $@

# special cases for the authorities' root certs:
authority/x509.pem: authority.template authority/secret.key
	certtool --generate-self-signed --load-privkey=authority/secret.key --template=authority.template > $@
rogueca/x509.pem: rogueca.template rogueca/secret.key
	certtool --generate-self-signed --load-privkey=rogueca/secret.key --template=rogueca.template > $@

%/cert-request: %.template %/secret.key 
	certtool --generate-request --load-privkey=$(dir $@)secret.key --template=$< > $@

%/x509.pem: %.template %/cert-request authority/secret.key authority/x509.pem 
	certtool --generate-certificate --load-ca-certificate=authority/x509.pem --load-ca-privkey=authority/secret.key --load-request=$(dir $@)cert-request --template=$< > $@

msva.gnupghome/trustdb.gpg: authority/minimal.pgp client/cert.pgp
	mkdir -p -m 0700 $(dir $@)
	GNUPGHOME=$(dir $@) gpg --import < $<
	printf "%s:6:\n" "$$(GNUPGHOME=authority gpg --with-colons --list-secret-keys --fingerprint | grep ^fpr: | cut -f 10 -d :)" | GNUPGHOME=$(dir $@) gpg --import-ownertrust
	GNUPGHOME=$(dir $@) gpg --import < client/cert.pgp
	printf "keyserver does-not-exist.example\n" > msva.gnupghome/gpg.conf


setup.done: $(all_tokens) msva.gnupghome/trustdb.gpg
	mkdir -p logs cache outputs
	touch setup.done


clean:
	rm -rf server client authority logs cache outputs setup.done server.template msva.gnupghome

.PHONY: all clean
