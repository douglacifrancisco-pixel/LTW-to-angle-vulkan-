#!/bin/bash
echo "=== CONFIGURAÇÃO DO GIT E GITHUB ==="
read -p "Digite seu nome de usuário do GitHub: " GITHUB_USER
read -p "Digite seu e-mail do GitHub: " GITHUB_EMAIL
echo "Cole seu Personal Access Token (PAT) do GitHub e pressione Enter:"
read -s GITHUB_TOKEN
echo ""

# 1. Identidade
git config --global user.name "$GITHUB_USER"
git config --global user.email "$GITHUB_EMAIL"

# 2. Cache de credenciais (caso o git peça novamente)
git config --global credential.helper store

# 3. Injeta o token na URL remota para evitar prompts futuros no Termux
git remote set-url origin https://$GITHUB_TOKEN@github.com/$GITHUB_USER/LTW-to-angle-vulkan-.git

echo "[OK] Configuração concluída. Tentando commit e push..."
git commit -m "fix: export EGL symbols for LWJGL dlsym compatibility"
git push origin feature/angle-vulkan-integration
