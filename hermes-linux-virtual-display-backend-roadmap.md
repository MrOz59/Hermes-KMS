# Hermes Linux Virtual Display Backend Roadmap

## 1. Objetivo

O objetivo deste documento é guiar o desenvolvimento de um novo backend de display virtual para o Hermes no Linux.

Hoje o Hermes consegue criar monitores virtuais usando EVDI, mas o EVDI não oferece um caminho ideal de baixa latência porque o fluxo tende a envolver framebuffer em memória acessível pela CPU/RAM, causando cópias extras antes do encode. O objetivo do novo backend é permitir um pipeline mais próximo de zero-copy, onde o frame renderizado possa permanecer em buffers compartilháveis pela GPU até chegar ao encoder de vídeo.

Este documento **não propõe abandonar o EVDI agora**. O EVDI deve continuar existindo como backend funcional e fallback de compatibilidade enquanto o novo backend DRM/KMS/DMA-BUF é desenvolvido, testado e amadurecido.

## 2. Objetivos principais

- Criar monitores virtuais no Linux sem dummy plug físico.
- Reduzir latência ao máximo possível.
- Evitar roundtrip desnecessário `GPU -> RAM/CPU -> GPU`.
- Usar hardware acceleration sempre que possível.
- Priorizar AMD e Intel inicialmente, por terem melhor integração com Mesa, DRM, GBM e VAAPI no Linux.
- Planejar suporte a NVIDIA desde o começo, mas implementar depois que o fluxo base estiver estável.
- Manter compatibilidade ampla com distros voltadas a jogos:
  - CachyOS
  - Arch Linux
  - Fedora
  - Bazzite
  - SteamOS
  - Nobara
  - Ubuntu-based gaming setups
- Manter EVDI como fallback até o backend novo ser robusto.

## 3. Situação atual

### 3.1 Backend EVDI

O EVDI resolve bem o problema de criar um display virtual funcional. Ele é útil porque:

- Já funciona em várias distros.
- Já é conhecido no ecossistema DisplayLink.
- Permite criar outputs virtuais sem dummy plug físico.
- Serve como backend compatível para casos onde o backend novo não está disponível.

Limitação principal:

```text
Game/Desktop render
    ↓
EVDI framebuffer / userspace path
    ↓
CPU/RAM copy or readback
    ↓
GPU upload or CPU encode
    ↓
Video encoder
    ↓
Stream
```

Isso adiciona latência, especialmente quando o objetivo é game streaming.

### 3.2 Backend desejado

O backend novo deve mirar em um fluxo semelhante a:

```text
Game/Desktop render
    ↓
DRM/KMS virtual output
    ↓
GPU-backed framebuffer
    ↓
DMA-BUF export/import
    ↓
Hardware encoder
    ↓
Stream
```

O objetivo é que o frame fique o máximo possível dentro do caminho GPU-native.

## 4. Conceitos importantes

### 4.1 DRM/KMS

DRM/KMS é a infraestrutura moderna do kernel Linux para gerenciamento de displays, modos, framebuffers, planes, CRTCs e connectors.

Para o Hermes, a ideia é criar um output virtual que pareça, para o compositor e para o sistema, um monitor real.

Componentes importantes:

- `drm_device`
- `drm_connector`
- `drm_encoder`
- `drm_crtc`
- `drm_plane`
- `drm_framebuffer`
- Atomic modesetting
- Page flip / vblank
- Framebuffer lifecycle

### 4.2 DMA-BUF

DMA-BUF permite compartilhar buffers entre drivers, dispositivos e processos usando file descriptors. É a peça central para tentar evitar cópias CPU/RAM.

No contexto do Hermes:

```text
Virtual display framebuffer
    ↓
DMA-BUF fd
    ↓
Encoder imports buffer
    ↓
Encode without CPU readback when supported
```

DMA-BUF por si só não garante zero-copy real. O driver, o formato do buffer, os modifiers e o encoder também precisam ser compatíveis.

### 4.3 PRIME / GEM / GBM

Esses componentes são relevantes para buffer allocation e sharing no ecossistema DRM/Mesa.

O backend novo provavelmente precisará lidar com:

- GEM buffer objects
- PRIME buffer sharing
- GBM allocations
- Buffer modifiers
- Explicit/implicit synchronization
- Fences

### 4.4 VAAPI / NVENC / AMF

O backend de display não é suficiente sozinho. O encoder precisa conseguir importar ou consumir o buffer sem cópia desnecessária.

Alvos:

```text
AMD / Intel:
DMA-BUF -> VAAPI

NVIDIA:
DMA-BUF/EGL/CUDA interop -> NVENC

Fallback:
DMA-BUF/framebuffer -> CPU copy -> encoder
```

## 5. Estratégia geral

O Hermes deve ter uma arquitetura de backends múltiplos.

```text
Hermes Display Backend API
├── evdi
│   └── fallback atual, compatível, sem promessa de zero-copy
│
├── pipewire/headless
│   └── backend sem módulo de kernel, útil para distros imutáveis
│
├── kms-capture
│   └── captura displays reais quando disponíveis
│
└── hermes-kms
    └── backend novo, objetivo final: virtual display GPU-native
```

Seleção automática sugerida:

```text
1. Usar hermes-kms se instalado, compatível e funcional.
2. Usar headless/gamescope/PipeWire se hermes-kms não estiver disponível.
3. Usar KMS capture de display real se existir output real.
4. Usar EVDI como fallback.
```

## 6. Princípios de desenvolvimento

1. Não quebrar o backend EVDI atual.
2. Não remover EVDI até o backend novo ser superior em estabilidade e latência.
3. Desenvolver o novo backend atrás de feature flag.
4. Começar com AMD/Intel.
5. Não começar por NVIDIA.
6. Medir latência em todas as fases.
7. Detectar se o caminho é realmente zero-copy ou se existe cópia escondida.
8. Aceitar fallback quando zero-copy não for possível.
9. Projetar para distros imutáveis desde o início.
10. Separar claramente:
    - criação de display virtual
    - captura de frame
    - export/import de buffer
    - encode
    - streaming

## 7. Roadmap de implementação

## Fase 0 — Preservar EVDI e preparar abstração

### Objetivo

Criar uma camada interna de backend de display para permitir que Hermes suporte vários backends sem acoplar toda a lógica ao EVDI.

### Tarefas

- Criar uma interface comum para backends de display.
- Mover lógica específica do EVDI para `backend_evdi`.
- Criar enum/configuração para seleção de backend.
- Adicionar logs claros indicando qual backend está ativo.
- Adicionar fallback automático para EVDI.
- Garantir que o comportamento atual continue funcionando.

### Exemplo de interface conceitual

```cpp
enum class DisplayBackendType {
    Auto,
    Evdi,
    PipeWireHeadless,
    KmsCapture,
    HermesKms
};

struct VirtualDisplayMode {
    uint32_t width;
    uint32_t height;
    uint32_t refreshRate;
};

struct FrameDescriptor {
    enum class Type {
        CpuMemory,
        DmaBuf,
        GpuHandle,
        Unknown
    };

    Type type;
    int dmaBufFd;
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t modifier;
};

class IDisplayBackend {
public:
    virtual ~IDisplayBackend() = default;

    virtual bool isAvailable() = 0;
    virtual bool createVirtualDisplay(const VirtualDisplayMode& mode) = 0;
    virtual bool destroyVirtualDisplay() = 0;
    virtual bool setMode(const VirtualDisplayMode& mode) = 0;
    virtual bool acquireFrame(FrameDescriptor& outFrame) = 0;
    virtual const char* name() const = 0;
};
```

### Critérios de aceite

- Hermes continua funcionando com EVDI.
- Backend pode ser escolhido por configuração/env var.
- Backend `auto` escolhe EVDI se nenhum outro estiver disponível.
- Logs mostram claramente o backend usado.

## Fase 1 — Medição do backend atual

### Objetivo

Criar baseline real antes de desenvolver o backend novo.

### Métricas obrigatórias

- Tempo de captura.
- Tempo de cópia CPU.
- Tempo de upload GPU, se houver.
- Tempo de encode.
- Latência fim-a-fim.
- Uso de CPU.
- Uso de GPU.
- Uso de memória.
- FPS entregue.
- Frame pacing / jitter.

### Tarefas

- Adicionar timestamps por frame.
- Criar logs agregados.
- Criar modo debug com métricas por etapa.
- Criar relatório simples no terminal.
- Comparar EVDI com display real/dummy plug se possível.

### Exemplo de log desejado

```text
[Hermes][Frame 10524]
backend=evdi
capture_ms=4.7
cpu_copy_ms=2.3
gpu_upload_ms=3.1
encode_ms=5.8
total_pipeline_ms=15.9
zero_copy=false
```

### Critérios de aceite

- É possível provar onde a latência está sendo adicionada.
- É possível comparar EVDI vs backend novo no futuro.
- Métricas funcionam sem quebrar release build.

## Fase 2 — Proof of Concept DMA-BUF -> VAAPI

### Objetivo

Antes de criar o monitor virtual novo, provar que o Hermes consegue pegar um DMA-BUF e encodar via VAAPI no caminho mais direto possível.

### Escopo inicial

- AMD Mesa.
- Intel Mesa.
- VAAPI.
- H.264 primeiro.
- H.265/HEVC depois.
- AV1 depois, se hardware suportar.

### Tarefas

- Criar módulo de encode capaz de importar DMA-BUF.
- Testar formatos:
  - NV12
  - P010
  - XR24/AR24 com conversão GPU-side, se necessário
- Detectar quando o import falhar.
- Fazer fallback para cópia CPU.
- Logar claramente se o encode é zero-copy provável ou fallback.

### Critérios de aceite

- Hermes consegue importar DMA-BUF e iniciar encode VAAPI.
- O caminho funciona em pelo menos uma máquina AMD ou Intel.
- O fallback funciona quando o import falha.
- Logs indicam:
  - formato
  - modifier
  - device
  - driver VAAPI
  - zero-copy/fallback

### Notas

Mesmo se o DMA-BUF for importado, pode haver cópia interna se o formato/modifier não for aceito diretamente pelo hardware. O Hermes deve tratar zero-copy como uma propriedade medida/detectada, não assumida.

## Fase 3 — Backend headless sem módulo de kernel

### Objetivo

Criar uma opção mais compatível para distros imutáveis ou ambientes onde instalar módulo de kernel é ruim.

Essa fase não substitui o `hermes-kms`, mas ajuda em SteamOS/Bazzite e reduz dependência de kernel module.

### Opções a investigar

- gamescope headless/session mode
- wlroots headless backend
- PipeWire screencast com DMA-BUF quando disponível
- KMS capture quando executando sob gamescope/game mode

### Fluxo conceitual

```text
Hermes session
    ↓
gamescope/wlroots/headless compositor
    ↓
game/desktop runs inside session
    ↓
capture via DMA-BUF/PipeWire/KMS path
    ↓
hardware encoder
    ↓
stream
```

### Vantagens

- Não precisa instalar módulo de kernel.
- Mais amigável para SteamOS/Bazzite.
- Bom para modo console/game mode.
- Pode funcionar melhor em sistemas imutáveis.

### Desvantagens

- Pode não aparecer como monitor virtual no desktop normal.
- Pode exigir rodar o jogo dentro de uma sessão especial.
- Integração com desktop existente pode ser limitada.
- Pode variar muito entre compositors.

### Critérios de aceite

- Hermes consegue iniciar uma sessão headless simples.
- Hermes consegue capturar frames da sessão.
- Hermes consegue encodar com VAAPI em AMD/Intel.
- Documentar limitações.

## Fase 4 — PoC `hermes-kms`

### Objetivo

Criar um módulo DRM/KMS virtual experimental que exponha um monitor virtual ao sistema.

Nome sugerido:

```text
hermes-kms
```

Artefatos sugeridos:

```text
kernel/hermes-kms/
userspace/hermes-kms-control/
```

O módulo deve expor um connector virtual:

```text
HERMES-1
```

### Referências técnicas

Estudar:

- VKMS do kernel Linux.
- DRM simple display drivers.
- DRM GEM helpers.
- Atomic modesetting.
- PRIME/DMA-BUF buffer sharing.
- Writeback connector concepts.

### Funcionalidade mínima da PoC

- Módulo carrega com `modprobe hermes-kms`.
- Sistema cria `/dev/dri/cardX`.
- Connector `HERMES-1` aparece em ferramentas como:
  - `modetest`
  - `drm_info`
  - KDE display settings
  - GNOME display settings, se aplicável
- Suporta pelo menos:
  - 1920x1080@60
- Aceita atomic modesetting.
- Permite desligar/remover output virtual.
- Não trava o compositor.

### Comandos úteis para teste

```bash
modetest -c
modetest -p
drm_info
ls -l /dev/dri/
journalctl -k -f
```

### Critérios de aceite

- O monitor virtual aparece no sistema.
- Um compositor consegue habilitar esse output.
- O sistema não congela ao ativar/desativar o output.
- O backend ainda não precisa ser zero-copy nesta fase.

## Fase 5 — Integração `hermes-kms` com userspace

### Objetivo

Permitir que o processo userspace do Hermes controle o backend `hermes-kms`.

### Possíveis interfaces

- ioctl próprio em `/dev/dri/cardX`
- sysfs/debugfs para debug
- netlink, se fizer sentido
- device node auxiliar, se necessário

### Funcionalidades esperadas

- Criar output virtual.
- Remover output virtual.
- Definir resolução.
- Definir refresh rate.
- Consultar estado.
- Receber ou acessar frame descriptor.
- Exportar DMA-BUF fd quando possível.
- Reportar erro/fallback.

### Exemplo conceitual

```bash
hermes-kmsctl create --width 1920 --height 1080 --refresh 60
hermes-kmsctl list
hermes-kmsctl destroy HERMES-1
```

### Critérios de aceite

- Hermes consegue criar/remover display virtual via API própria.
- CLI de debug funciona.
- Logs do kernel e userspace são claros.

## Fase 6 — DMA-BUF real no `hermes-kms`

### Objetivo

Fazer com que o framebuffer/output virtual possa ser compartilhado com o encoder via DMA-BUF.

### Tarefas técnicas

- Implementar ou reutilizar GEM objects apropriados.
- Suportar PRIME export/import quando aplicável.
- Preservar metadata:
  - width
  - height
  - stride
  - pixel format
  - modifier
  - plane count
- Lidar com synchronization/fences.
- Garantir que o buffer seja válido até o encoder terminar.
- Implementar ring/buffer queue para evitar bloquear compositor.
- Evitar CPU mmap no caminho rápido.

### Fluxo desejado

```text
Compositor renders to Hermes output
    ↓
Hermes KMS exposes frame as DMA-BUF
    ↓
Hermes userspace imports fd
    ↓
VAAPI encoder consumes fd
    ↓
Network stream
```

### Pontos difíceis

- O compositor pode renderizar em formato inadequado.
- O encoder pode não aceitar o modifier.
- Pode ser necessário converter RGB para NV12.
- Conversão precisa ser GPU-side para preservar baixa latência.
- Synchronization errada pode causar tearing, frame antigo ou corruption.
- Buffer lifetime precisa ser muito bem controlado.

### Critérios de aceite

- O Hermes recebe frame como DMA-BUF.
- O encoder tenta importar sem CPU copy.
- Fallback funciona quando import falha.
- Métricas indicam redução real de latência vs EVDI.
- Não há vazamento de fd/buffer.

## Fase 7 — Conversão GPU-side

### Objetivo

Resolver o problema de formatos incompatíveis entre compositor e encoder.

Muitos compositors entregam frames RGB/XRGB/ARGB, mas encoders normalmente preferem NV12/P010.

### Estratégias possíveis

- VAAPI VPP para conversão.
- Vulkan compute.
- OpenGL/EGL shader path.
- libplacebo, se fizer sentido.
- GStreamer pipeline experimental, apenas para validação.

### Fluxo desejado

```text
DMA-BUF RGB
    ↓
GPU-side color conversion
    ↓
NV12/P010 GPU surface
    ↓
VAAPI/NVENC/AMF encode
```

### Critérios de aceite

- Conversão não usa CPU readback.
- Conversão funciona em AMD/Intel.
- Latência é medida.
- Qualidade visual é aceitável.
- Fallback CPU existe, mas é marcado como fallback.

## Fase 8 — NVIDIA support planejado

### Objetivo

Adicionar suporte a NVIDIA depois que AMD/Intel estiverem estáveis.

NVIDIA deve ser planejada desde o começo, mas não deve bloquear o PoC inicial.

### Caminhos a investigar

- NVENC.
- CUDA interop.
- EGL/GBM interop.
- DMA-BUF import support no driver proprietário.
- Vulkan Video futuramente, se ficar maduro e útil.
- Fallback via CPU copy quando import direto não for possível.

### Cuidados

- NVIDIA proprietary driver muda comportamento entre versões.
- GBM melhorou, mas ainda pode ter diferenças em relação a Mesa.
- Nem todo formato/modifier será importável.
- Wayland/compositor/NVIDIA ainda pode ter bugs específicos.
- Não assumir que o mesmo caminho AMD/Intel vai funcionar.

### Critérios de aceite futuro

- Detectar NVIDIA corretamente.
- Escolher NVENC.
- Tentar caminho zero-copy/import direto.
- Fazer fallback limpo.
- Logar claramente o motivo do fallback.
- Não quebrar AMD/Intel.

## Fase 9 — Empacotamento e distribuição

### Objetivo

Garantir compatibilidade com o maior número possível de distros.

### Estratégia por tipo de distro

#### Arch / CachyOS

- Pacote AUR.
- DKMS para `hermes-kms`.
- Pacote userspace separado.
- Integração com systemd user service.

#### Fedora / Nobara

- RPM.
- DKMS ou akmods.
- SELinux notes, se necessário.
- systemd user service.

#### Bazzite

- Evitar depender exclusivamente de kernel module.
- Suportar backend headless/gamescope.
- Documentar limitações.
- Avaliar integração com `ujust`/rpm-ostree layering.
- Fornecer fallback EVDI/headless.

#### SteamOS

- Tratar como plataforma sensível.
- Evitar exigir kernel module como único caminho.
- Priorizar backend headless/gamescope.
- EVDI pode ser fallback se suportado.
- Documentar claramente o que exige developer mode ou alterações no sistema.

#### Ubuntu / Debian-based

- Pacote `.deb`.
- DKMS.
- systemd user service.
- Dependências claras:
  - kernel headers
  - libdrm
  - mesa
  - libva
  - vainfo
  - ffmpeg or internal encoder stack

### Critérios de aceite

- Instalação documentada para pelo menos Arch/CachyOS e Fedora/Bazzite.
- O Hermes não quebra se o módulo não compilar.
- Backend EVDI continua disponível.
- Mensagem de erro orienta o usuário.

## Fase 10 — Testes

### Matriz inicial de teste

| GPU | Driver | Distro | Backend alvo |
|---|---|---|---|
| AMD RDNA2/RDNA3 | Mesa | CachyOS | hermes-kms + VAAPI |
| AMD RDNA2/RDNA3 | Mesa | Bazzite | headless/gamescope + VAAPI |
| Intel Xe/iGPU | Mesa | Fedora | hermes-kms + VAAPI |
| Intel Xe/iGPU | Mesa | Arch | hermes-kms + VAAPI |
| NVIDIA RTX | proprietary | Arch/CachyOS | planned NVENC |
| NVIDIA RTX | proprietary | Bazzite/Fedora | planned NVENC/fallback |

### Testes funcionais

- Criar display virtual.
- Remover display virtual.
- Trocar resolução.
- Trocar refresh rate.
- Reiniciar compositor.
- Suspender/retomar sistema.
- Logout/login.
- Multi-monitor real + virtual.
- Game fullscreen.
- Game borderless.
- Desktop capture.
- Gamescope session.
- KDE Wayland.
- GNOME Wayland.
- X11, se suportado ou fallback.

### Testes de performance

- 1080p60.
- 1080p120.
- 1440p60.
- 1440p120.
- 4K60.
- Latência média.
- Latência p95/p99.
- FPS perdido.
- Frame pacing.
- CPU usage.
- GPU encode usage.
- GPU render impact.
- Memory bandwidth impact.

### Testes de estabilidade

- Rodar por 1h.
- Rodar por 8h.
- Criar/destruir display 100 vezes.
- Trocar resolução 100 vezes.
- Encerrar Hermes durante stream.
- Crash do encoder.
- Crash do compositor.
- Remover módulo com display ativo.
- Atualizar driver/kernel.

## 8. Downsides e riscos

### 8.1 Complexidade de kernel

Criar um driver DRM/KMS é trabalho sério. Erros podem causar:

- tela preta
- travamento do compositor
- kernel warnings
- memory leaks
- fd leaks
- buffer corruption
- problemas após update de kernel

Mitigação:

- Começar experimental.
- Manter EVDI fallback.
- Testar em Arch/CachyOS primeiro.
- Não habilitar por padrão até estabilizar.

### 8.2 Secure Boot

Módulos DKMS podem falhar com Secure Boot ativo se não forem assinados.

Mitigação:

- Documentar assinatura de módulo.
- Detectar falha e cair para fallback.
- Fornecer backend sem kernel module.

### 8.3 Distros imutáveis

SteamOS/Bazzite podem dificultar instalação de kernel module.

Mitigação:

- Backend headless/gamescope.
- EVDI fallback.
- Documentação específica.
- Não depender exclusivamente de `hermes-kms`.

### 8.4 Zero-copy pode falhar silenciosamente

Mesmo usando DMA-BUF, o driver pode copiar internamente.

Mitigação:

- Métricas.
- Logs detalhados.
- Detectar formato/modifier incompatível.
- Reportar fallback.

### 8.5 NVIDIA

NVIDIA pode exigir caminhos específicos.

Mitigação:

- Planejar interface genérica.
- Implementar depois.
- Não misturar código NVIDIA no PoC AMD/Intel.
- Logar claramente suporte experimental.

## 9. Estrutura sugerida do projeto

```text
hermes/
├── src/
│   ├── display/
│   │   ├── IDisplayBackend.hpp
│   │   ├── DisplayBackendFactory.cpp
│   │   ├── evdi/
│   │   │   └── EvdiBackend.cpp
│   │   ├── headless/
│   │   │   └── HeadlessBackend.cpp
│   │   ├── kms/
│   │   │   ├── HermesKmsBackend.cpp
│   │   │   └── HermesKmsControl.cpp
│   │   └── common/
│   │       ├── FrameDescriptor.hpp
│   │       └── DisplayMode.hpp
│   │
│   ├── encode/
│   │   ├── IEncoder.hpp
│   │   ├── vaapi/
│   │   │   └── VaapiEncoder.cpp
│   │   ├── nvenc/
│   │   │   └── NvencEncoder.cpp
│   │   └── fallback/
│   │       └── CpuEncoder.cpp
│   │
│   └── metrics/
│       └── FrameMetrics.cpp
│
├── kernel/
│   └── hermes-kms/
│       ├── Makefile
│       ├── dkms.conf
│       ├── hermes_kms_drv.c
│       ├── hermes_kms_connector.c
│       ├── hermes_kms_plane.c
│       ├── hermes_kms_crtc.c
│       ├── hermes_kms_gem.c
│       └── hermes_kms_uapi.h
│
├── tools/
│   └── hermes-kmsctl/
│       └── main.cpp
│
└── docs/
    ├── linux-virtual-display-backend.md
    ├── hermes-kms-debugging.md
    └── compatibility-matrix.md
```

## 10. Feature flags/configuração

### Variáveis de ambiente sugeridas

```bash
HERMES_DISPLAY_BACKEND=auto
HERMES_DISPLAY_BACKEND=evdi
HERMES_DISPLAY_BACKEND=headless
HERMES_DISPLAY_BACKEND=hermes-kms

HERMES_FORCE_CPU_COPY=0
HERMES_DEBUG_FRAME_TIMING=1
HERMES_DISABLE_ZERO_COPY=0
HERMES_PREFER_VAAPI=1
HERMES_PREFER_NVENC=1
```

### Configuração sugerida

```toml
[display]
backend = "auto"
preferred_width = 1920
preferred_height = 1080
preferred_refresh = 60
allow_evdi_fallback = true
allow_headless_fallback = true

[zero_copy]
enabled = true
allow_gpu_conversion = true
allow_cpu_fallback = true

[encoder]
preferred = "auto"
amd_intel = "vaapi"
nvidia = "nvenc"
fallback = "software"
```

## 11. Critérios para considerar o backend novo bem-sucedido

O backend `hermes-kms` só deve ser considerado pronto para uso padrão quando:

- Criar/remover monitor virtual for estável.
- AMD/Intel funcionarem com VAAPI.
- Fallback para EVDI funcionar sem intervenção.
- Latência for comprovadamente menor que EVDI.
- Logs diagnosticarem claramente zero-copy vs fallback.
- Instalação for razoável em pelo menos Arch/CachyOS e Fedora.
- Não causar crash em KDE/GNOME/gamescope.
- Tiver documentação clara para distros imutáveis.
- Tiver plano de suporte NVIDIA em andamento.

## 12. Ordem recomendada para o Codex executar

### Passo 1

Criar a abstração `IDisplayBackend` e mover o EVDI atual para `EvdiBackend`.

### Passo 2

Adicionar `FrameMetrics` e instrumentar o pipeline atual.

### Passo 3

Criar `FrameDescriptor` com suporte a CPU memory e DMA-BUF.

### Passo 4

Criar skeleton de `HermesKmsBackend`, mesmo que ainda retorne unavailable.

### Passo 5

Criar pasta `kernel/hermes-kms` com Makefile, DKMS config e módulo mínimo que carrega/descarrega.

### Passo 6

Implementar PoC de connector virtual inspirado em VKMS.

### Passo 7

Criar `hermes-kmsctl` para debug.

### Passo 8

Criar PoC VAAPI importando DMA-BUF.

### Passo 9

Integrar `HermesKmsBackend` com o módulo.

### Passo 10

Adicionar fallback automático para EVDI.

### Passo 11

Adicionar documentação de instalação para Arch/CachyOS.

### Passo 12

Testar Fedora/Bazzite com backend headless/fallback.

### Passo 13

Planejar e iniciar suporte NVIDIA/NVENC.

## 13. Decisão importante

Não tentar transformar EVDI no backend zero-copy principal.

EVDI deve permanecer como:

```text
compatibility backend
```

O novo backend deve ser tratado como:

```text
low-latency GPU-native backend
```

Essa separação evita quebrar usuários atuais e permite evoluir a arquitetura sem carregar limitações estruturais do EVDI.

## 14. Resumo final

O Hermes deve evoluir de:

```text
virtual monitor that provides pixels
```

para:

```text
virtual GPU-native display output for low-latency streaming
```

A estratégia recomendada é:

1. Preservar EVDI.
2. Criar abstração de backend.
3. Medir o pipeline atual.
4. Provar DMA-BUF -> VAAPI em AMD/Intel.
5. Criar backend headless sem módulo para compatibilidade.
6. Criar `hermes-kms` como backend DRM/KMS virtual.
7. Integrar DMA-BUF real.
8. Adicionar conversão GPU-side.
9. Depois implementar NVIDIA/NVENC.
10. Manter fallback robusto em todos os casos.

O sucesso do projeto depende de não vender “zero-copy” como promessa absoluta. O Hermes deve detectar, medir e reportar quando o caminho é realmente acelerado e quando caiu para fallback.
