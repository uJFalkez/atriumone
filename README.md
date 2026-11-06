# AtriumOne

Projeto de TCC para captura de imagens RGB e NIR com posição e orientação,
cálculo de NDVI, detecção de focos de NDVI baixo e visualização das missões
num mapa de satélite. O repositório reúne quatro módulos: `raspberry`,
`alignment`, `server` e `webApp`.

O contrato entre o módulo e a estação (API, estados da missão e formato do
`metadata.json`) está em [`server/CONTRATO.md`](server/CONTRATO.md).
O passo a passo do que o módulo deve fazer, e do que não deve, está em
[`PIPELINE.md`](PIPELINE.md).

## Pipeline

1. **Comando — `webApp/`:** o dashboard consulta o módulo por polling. Com o
   módulo conectado, ele libera os botões **Calibrar sensores** e **Iniciar
   missão**.
2. **Missão — módulo:** o módulo zera o referencial na estação. Após 2 min de
   estabilização, tira uma foto (RGB + NIR crus + `metadata.json`) a cada
   10 s, sempre que estiver estável. Ao voltar à estação (perto do ponto
   inicial, baixo, parado e alcançando a API da estação), reporta
   `ready_to_finish` e o dashboard libera **Finalizar missão**.
3. **Envio — módulo:** ao finalizar, o módulo registra a missão na estação
   (`POST /mission`, com o histórico de posições) e envia cada captura para
   `POST /entry`. A cópia local é apagada após cada confirmação.
4. **Processamento — `server/`:** a estação grava os arquivos crus de forma
   atômica e então:
   - alinha o NIR à RGB (`alignment/alignment.npy`);
   - calcula o NDVI;
   - detecta focos pequenos de NDVI baixo;
   - projeta a pegada da foto e os focos no solo;
   - gera as prévias para o dashboard.
5. **Visualização — `webApp/`:** o mapa de satélite mostra as fotos
   projetadas (NDVI ou RGB), o trajeto e os focos. Os painéis trazem
   comparação RGB/NDVI, histograma, telemetria e o resumo de cada missão.

## Módulos

### `raspberry/` — módulo embarcado

Código legado da primeira versão (NDVI calculado no Pi). O módulo está sendo
reescrito para seguir [`server/CONTRATO.md`](server/CONTRATO.md); a estação
não depende deste código.

### `alignment/` — calibração RGB/NIR

Contém `align.py`, as imagens de referência e a matriz de alinhamento. O
script estima uma transformação afim entre as imagens RGB e NIR e grava a
matriz no caminho informado. A estação lê `alignment/alignment.npy`.

```bash
python3 alignment/align.py alignment/rgb.jpg alignment/nir.jpg alignment/alignment.npy
```

### `server/` — API de ingestão e processamento

API Flask que recebe as capturas e processa cada uma em segundo plano.
As capturas ficam em `entries/<uuid>/` (configurável por `ENTRIES_DIR`). O
registro de cada missão fica em `missions/<uuid>/` (`MISSIONS_DIR`), com
`mission.json` e `track.csv`.

```bash
python3 -m pip install -r server/requirements.txt
cd server
waitress-serve --host=0.0.0.0 --port=5000 app:app
```

- `config.py` reúne os parâmetros de processamento: recorte, FOV da câmera
  (`CAMERA_HFOV_DEG`) e limiares da detecção de focos.
- `python reprocess.py [--all | <uuid>…]` refaz o processamento, por exemplo
  depois de ajustar os parâmetros.
- `python tools/mock_module.py` sobe um **módulo simulado** que implementa o
  contrato. Ele voa sobre um campo sintético com focos plantados e envia as
  capturas, o que permite usar o dashboard sem o Raspberry. Com
  `MOCK_SPEED=8`, o simulador acelera o tempo.

### `webApp/` — dashboard da estação

Aplicação Next.js que comanda o módulo e apresenta as missões.

```bash
cd webApp
npm ci
npm run dev   # procura o módulo em http://pi3b:8000 (Tailscale)
```

| Variável | Padrão | Uso |
|---|---|---|
| `MODULE_URL` | `http://pi3b:8000` | API do módulo (hostname do Raspberry na tailnet) |
| `ENTRIES_DIR` | `../entries` | Capturas recebidas pela estação |
| `MISSIONS_DIR` | `../missions` | Registro das missões: o do módulo (`mission.json`, `track.csv`) e o do dashboard (`record.json`) |
| `STATION_FILE` | `station.json` (dentro de `webApp/`) | Posição da estação, marcada ou removida no próprio mapa. Só o dashboard usa. |

Na primeira execução, marque a posição da estação no mapa: as posições do
módulo são relativas ao ponto de partida. Para produção, use
`npm run build` e depois `npm start`.

Para testar tudo localmente sem o módulo, em três terminais:

```bash
cd server && waitress-serve --port=5000 app:app
cd server && MOCK_SPEED=8 MOCK_PORT=8000 python tools/mock_module.py
cd webApp && MODULE_URL=http://127.0.0.1:8000 npm run dev
```
