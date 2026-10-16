#!/bin/bash
# Test certificates for sx_net_server.py: one CA, several leaves.
# usage: sx_net_certs.sh <outdir>
set -e
D=$1
mkdir -p "$D"
cd "$D"

openssl ecparam -name prime256v1 -genkey -noout -out ca.key
openssl req -x509 -new -key ca.key -sha256 -days 3650 -subj "/CN=SX Test CA" \
    -addext "basicConstraints=critical,CA:TRUE" -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.pem

leaf() {   # name keyspec san [extra x509 args]
    local name=$1 keyspec=$2 san=$3; shift 3
    if [ "$keyspec" = rsa ]; then openssl genrsa -out $name.key 2048 2>/dev/null
    else openssl ecparam -name prime256v1 -genkey -noout -out $name.key; fi
    openssl req -new -key $name.key -subj "/CN=$san" -out $name.csr
    printf "subjectAltName=DNS:%s\nbasicConstraints=CA:FALSE\nkeyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n" "$san" > $name.ext
    openssl x509 -req -in $name.csr -CA ca.pem -CAkey ca.key -CAcreateserial -sha256 -days 365 \
        -extfile $name.ext "$@" -out $name.pem 2>/dev/null
}

leaf ec ec localhost
leaf rsa rsa localhost
leaf wronghost ec other.example
leaf expired ec localhost -not_before 20200101000000Z -not_after 20200102000000Z

# Self-signed, not chained to the test CA.
openssl ecparam -name prime256v1 -genkey -noout -out self.key
openssl req -x509 -new -key self.key -sha256 -days 365 -subj "/CN=localhost" \
    -addext "subjectAltName=DNS:localhost" -out self.pem

for f in ec rsa wronghost expired; do openssl verify -CAfile ca.pem $f.pem >/dev/null 2>&1 && echo "cert $f ok" || echo "cert $f: verify says (expected for expired) $(openssl verify -CAfile ca.pem $f.pem 2>&1 | tail -1)"; done
