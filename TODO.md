# Backlog & Roadmap: Bonfire Pomodoro

Tarefas técnicas priorizadas para aprimoramento e manutenção contínua do utilitário.

## Em Andamento 🚀

- [ ] **Compatibilidade Nativa com macOS (Homebrew)**
  - Áudio nativo usando `afplay` como reprodutor de sistema (oneshots e loop ambiente)
  - Notificações de desktop nativas no macOS via `osascript`
  - Matriz multiplataforma no GitHub Actions CI (`ubuntu-latest` e `macos-latest`)

## Próximas Tarefas (Backlog Priorizado) 📋

- [ ] **Usabilidade CLI e Padrão POSIX**
  - Implementar flag `--help` / `-h` detalhando todas as opções de execução
  - Implementar flag `--version` / `-v` exibindo a versão e metadados de build
  - Desacoplar `-h` de `--history` para preservar a convenção Unix padrão de help

- [ ] **Conformidade com a Especificação XDG Base Directory**
  - Checar `$XDG_CONFIG_HOME` antes do fallback para `$HOME/.config/bonfire/history.log`

- [ ] **Man Page Unix**
  - Criar página de manual `bonfire.1` em formato troff
  - Adicionar regra de instalação da manpage no `Makefile` (`$(PREFIX)/share/man/man1`)

- [ ] **Empacotamento e Distribuição**
  - [x] Homebrew Tap oficial (`GabrielBaiano/homebrew-tap`)
  - [ ] Pacote AUR mantido no Arch User Repository (`packaging/aur`)
