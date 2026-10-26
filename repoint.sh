#!/bin/sh
# Re-point the platform-managed supervisor entry to this v2 stack.
# Run this after a platform push-to-deploy resets ~/deploy/unisso/deploy.sh.
set -eu
cat > /home/cloud/deploy/unisso/deploy.sh <<'EOF'
#!/bin/sh
# v2 stack: Vue 3 SPA + C++ backend. Source lives in ~/deploy/unisso-v2.
exec sh /home/cloud/deploy/unisso-v2/deploy.sh
EOF
chmod +x /home/cloud/deploy/unisso/deploy.sh
~/.local/bin/supervisorctl -c ~/deploy/supervisord.conf restart deploy-5829cb069e42fd20
