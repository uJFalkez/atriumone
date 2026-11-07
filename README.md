# AtriumOne

Projeto de TCC para captura de imagens RGB e NIR com posição e orientação,
cálculo de NDVI, detecção de focos de NDVI baixo e visualização das missões
num mapa de satélite.

```
            comandos + polling de status (GET /status a cada 2 s)
 webApp ──────────────────────────────────────────────▶ module (pi3b:8000)
 (dashboard)                                               │ POST /entry (cada foto, inclusive em voo)
      ▲                                                    │ POST /mission (registro + trajeto, no fim)
      │ lê entries/ e missions/                            ▼
      └──────────────────────────────────────────── server (station:5000)
                                                     alinha NIR→RGB, NDVI, focos, georreferência
```

| Pasta | O que é |
|---|---|
| [`module/`](module/) | Software do módulo embarcado (Raspberry Pi 3B, hostname `pi3b`): sensores, navegação, missão, câmeras e envio. |
| [`server/`](server/) | API da estação (Flask): recebe missões e capturas e faz todo o processamento. |
| [`webApp/`](webApp/) | Dashboard da estação (Next.js): comanda o módulo e mostra as missões no mapa. |

A versão anterior (NDVI calculado no próprio Pi) está na tag `V1`.

Ficam **fora do git**, só na máquina da estação: `docs/` (contrato
módulo↔estação, pipeline do módulo, notas do dashboard e do módulo),
`entries/` e `missions/` (dados recebidos), `logs/`, `report/` e `.venv/`.

## Rede

Módulo e estação conversam pela tailnet (Tailscale): o módulo em
`http://pi3b:8000` e a estação em `http://station:5000`. Os dois serviços
escutam em `0.0.0.0`.

## Missão, de ponta a ponta

1. **Comando:** o dashboard consulta `GET /status` do módulo sem parar. Com o
   módulo conectado, libera **Calibrar sensores** e **Iniciar missão**.
2. **Missão:** o módulo zera o referencial na estação (posição norte/leste e
   altura relativas ao ponto de partida). Após 120 s de aquecimento, tira uma
   foto (RGB + NIR crus + `metadata.json`) a cada 10 s e grava o trajeto a
   5 Hz. O critério de estabilidade existe, mas está desativado: em voo o
   drone nunca ficou abaixo do limiar.
3. **Envio:** sempre que alcança a estação, o módulo envia as fotos
   (`POST /entry`) e apaga a cópia local após a confirmação. Ao voltar
   (perto do início, baixo e parado, ou pousado sem GPS), reporta
   `ready_to_finish`; o operador finaliza pelo dashboard e o módulo envia o
   que falta e, por último, o registro da missão (`POST /mission`).
4. **Processamento (estação):** cada foto é gravada de forma atômica e
   processada em segundo plano (detalhes em `server/`).
5. **Visualização:** mapa de satélite com o trajeto e os pontos das fotos,
   coloridos pelo NDVI médio; a foto aparece no hover. Painéis com comparação
   RGB/NDVI, histograma, focos, telemetria e o resumo de cada missão.

## `module/` — Raspberry Pi

| Peça | Ligação |
|---|---|
| IMU MPU6500 (accel + gyro) | I2C, 0x68, operado a 50 Hz |
| Magnetômetro QMC5883P | I2C, 0x2C (mesmo barramento; lido em rajadas) |
| GPS NEO-6M | UART `/dev/ttyS0`, 9600 baud (instável; opcional) |
| Câmera RGB Arducam 8MP USB | 1600×1200 MJPEG, campo de visão inteiro |
| Câmera NIR Pi Camera V2 NoIR (CSI) | 1640×1232 (binning 2×2), montada invertida (girada 180° por software) |

Dois serviços systemd:

- **`navd`** (C, `nav/`, `drivers/`): único dono do I2C e da UART. Filtro
  mediana, calibração, atitude (Mahony), Kalman de posição/velocidade (GPS
  quando há fix, ZUPT quando pousado), detectores de estável/parado/pousado e
  a sessão de calibração guiada (`calibration/`).
- **`missiond`** (Python, `mission/`): API HTTP `:8000`, estados e
  persistência em `data/state.json`, trajeto (`track.csv`), capturas (RGB via
  `cam/camgrab`, NIR via `rpicam-still`) e o uploader (processo filho de baixa
  prioridade). Conversa com o `navd` por socket Unix.

Dependências no Pi (Raspberry Pi OS com I2C e UART habilitados): `gcc`, `make`
e `libcjson-dev` para o código C; o `missiond` usa só a biblioteca padrão do
Python; a NIR usa o `rpicam-still` (já incluso no sistema).

```bash
sudo apt install build-essential libcjson-dev
make && make test                  # compila e roda os testes sintéticos
sudo deploy/install.sh --now       # instala e inicia navd + missiond (systemd)
sudo systemctl restart navd missiond
journalctl -u navd -u missiond -f  # logs
curl -s localhost:8000/status      # status do módulo
```

Configuração em `mission/config.py` (aquecimento, intervalo entre fotos,
retorno, câmeras: exposição RGB fixa de 1 ms, ganhos AWB da NIR fixos em
1,0/1,5). Ferramenta das câmeras: `tools/cam_calibrate.py focus|awb`.
`build/`, `data/` e `logs/` são gerados no Pi e não são versionados.

Limitações conhecidas do hardware: a NoIR **não tem filtro passa-IR** (o
NDVI calculado é um pseudo-NDVI); a calibração do acelerômetro nunca
completou; a montagem IMU→câmera não foi medida; o NEO-6M perde o fix com
frequência, e sem ele a posição por IMU deriva.

## `server/` — estação: ingestão e processamento

Na primeira vez, crie o ambiente virtual na raiz do projeto (o Ubuntu não
permite `pip install` no Python do sistema):

```bash
python3 -m venv .venv
.venv/bin/pip install -r server/requirements.txt
```

Para subir a API, sempre com o ambiente ativado (o prompt mostra `(.venv)`):

```bash
source .venv/bin/activate
cd server
waitress-serve --host=0.0.0.0 --port=5000 app:app
```

Endpoints: `GET /health`, `POST /entry` (rgb.jpg + nir.jpg + metadata.json,
idempotente) e `POST /mission` (mission.json + track.csv, idempotente). Os
dados ficam em `entries/<uuid>/` e `missions/<uuid>/` na raiz do projeto
(`ENTRIES_DIR`, `MISSIONS_DIR`).

Processamento de cada foto (`processing.py`), com resultado em
`analysis.json`, `ndvi.npy` e `preview/`:

1. **Alinhamento NIR→RGB:** matriz afim calibrada com alvos no chão
   (`calibration/`), escolhida pelo foco reportado da RGB, e refinada por foto
   com ECC dentro de limites plausíveis. Erro medido nos alvos: ~1,9 px.
2. **NDVI** na região coberta pelas duas câmeras.
3. **Qualidade:** ficam fora do levantamento (sem focos, fora do mapa e das
   médias) as fotos abaixo de 5 m e as com RGB estourada.
4. **Focos:** regiões pequenas (≥ 0,05 m²) com queda de NDVI ≥ 0,12 em
   relação à vegetação em volta; áreas grandes, alongadas ou sem vegetação ao
   redor são descartadas.
5. **Georreferência:** pegada da foto e posição dos focos em metros
   norte/leste, projetando os raios da câmera no solo com altura e
   orientação (HFOV da RGB ≈ 60,8°).

Ferramentas (de dentro de `server/`):

- `python reprocess.py [--all | <uuid>…]`: refaz o processamento (por
  exemplo, depois de mudar `config.py`).
- `python tools/align_targets.py ../entries <missão> calibration/<arquivo>.npy`:
  recalibra o alinhamento com fotos que tenham os alvos; `--relative-to`
  ajusta só escala e translação sobre outra matriz. Depois, rode
  `reprocess.py --all`.
- `python tools/mock_module.py`: módulo simulado que implementa a API do
  módulo sobre um campo sintético, para testar sem o Raspberry
  (`MOCK_SPEED=8` acelera o tempo).

## `webApp/` — dashboard

```bash
cd webApp
npm ci
npm run dev                  # desenvolvimento
npm run build && npm start   # produção (recomendado para operar em campo)
```

| Variável | Padrão | Uso |
|---|---|---|
| `MODULE_URL` | `http://pi3b:8000` | API do módulo |
| `ENTRIES_DIR` | `../entries` | Capturas recebidas pela estação |
| `MISSIONS_DIR` | `../missions` | Registro das missões (do módulo e do dashboard) |
| `STATION_FILE` | `webApp/station.json` | Posição da estação, marcada ou removida no próprio mapa |

Na primeira execução, marque a posição da estação no mapa: as posições do
módulo são relativas ao ponto de partida. Em modo dev, o Next só aceita
acesso por `localhost` e pelos nomes da tailnet listados em `next.config.ts`.

## Testar sem o módulo

Em três terminais (nos dois primeiros, com `source .venv/bin/activate`):

```bash
cd server && waitress-serve --port=5000 app:app
cd server && MOCK_SPEED=8 MOCK_PORT=8000 python tools/mock_module.py
cd webApp && MODULE_URL=http://127.0.0.1:8000 npm run dev
```
