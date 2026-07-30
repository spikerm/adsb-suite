#!/usr/bin/env bash
set -euo pipefail

CERT_DIR=/etc/adsb-suite/tls
PUBLIC_DIR=/var/lib/adsb-suite/public
PUBLIC_CA="$PUBLIC_DIR/adsb-suite-ca.crt"
NGINX_SITE=/etc/nginx/sites-available/adsb-suite-https
NGINX_LINK=/etc/nginx/sites-enabled/adsb-suite-https
IP="${ADSB_SUITE_IP:-$(hostname -I | awk '{print $1}')}"
HOST_FQDN="$(hostname -f 2>/dev/null || hostname)"
HOST_SHORT="$(hostname)"

[ -n "$IP" ] || { echo "Geen lokaal IP-adres gevonden." >&2; exit 1; }
mkdir -p "$CERT_DIR" "$PUBLIC_DIR"
chmod 750 "$CERT_DIR"
chmod 755 "$PUBLIC_DIR"

CA_KEY="$CERT_DIR/adsb-suite-ca.key"
CA_CRT="$CERT_DIR/adsb-suite-ca.crt"
SERVER_KEY="$CERT_DIR/adsb-suite.key"
SERVER_CSR="$CERT_DIR/adsb-suite.csr"
SERVER_CRT="$CERT_DIR/adsb-suite.crt"
EXT="$CERT_DIR/adsb-suite.ext"

if [ ! -s "$CA_KEY" ] || [ ! -s "$CA_CRT" ]; then
  openssl genrsa -out "$CA_KEY" 4096
  openssl req -x509 -new -nodes -key "$CA_KEY" -sha256 -days 3650 \
    -subj "/CN=ADS-B Suite Local CA/O=ADS-B Suite" -out "$CA_CRT"
fi

cat > "$EXT" <<EOF
basicConstraints=CA:FALSE
keyUsage=digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=IP:${IP},DNS:${HOST_SHORT},DNS:${HOST_FQDN},DNS:localhost
EOF

openssl genrsa -out "$SERVER_KEY" 2048
openssl req -new -key "$SERVER_KEY" -subj "/CN=${IP}/O=ADS-B Suite" -out "$SERVER_CSR"
openssl x509 -req -in "$SERVER_CSR" -CA "$CA_CRT" -CAkey "$CA_KEY" -CAcreateserial \
  -out "$SERVER_CRT" -days 825 -sha256 -extfile "$EXT"

chown root:root "$CA_KEY" "$CA_CRT" "$SERVER_KEY" "$SERVER_CRT"
chmod 600 "$CA_KEY" "$SERVER_KEY"
chmod 644 "$CA_CRT" "$SERVER_CRT"
install -m 644 -o root -g root "$CA_CRT" "$PUBLIC_CA"

cat > "$NGINX_SITE" <<EOF
server {
    listen 8443 ssl;
    listen [::]:8443 ssl;
    server_name _;

    ssl_certificate     ${SERVER_CRT};
    ssl_certificate_key ${SERVER_KEY};
    ssl_protocols TLSv1.2 TLSv1.3;
    ssl_session_cache shared:ADSBSSL:10m;

    location = /adsb-suite-ca.crt {
        alias ${PUBLIC_CA};
        default_type application/x-x509-ca-cert;
        add_header Content-Disposition 'attachment; filename="adsb-suite-ca.crt"';
        add_header Cache-Control 'no-store';
    }

    location / {
        proxy_pass http://127.0.0.1:8090;
        proxy_http_version 1.1;
        proxy_set_header Host \$host;
        proxy_set_header X-Real-IP \$remote_addr;
        proxy_set_header X-Forwarded-For \$proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto https;
        proxy_set_header Upgrade \$http_upgrade;
        proxy_set_header Connection "upgrade";
        proxy_read_timeout 3600;
    }
}
EOF

ln -sf "$NGINX_SITE" "$NGINX_LINK"
nginx -t
systemctl enable --now nginx
systemctl reload nginx

echo "HTTPS actief: https://${IP}:8443/"
echo "CA-certificaat: https://${IP}:8443/adsb-suite-ca.crt"