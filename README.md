# AtriumOne

Projeto de TCC para captura de imagens RGB e NIR, processamento de NDVI e
visualização de operações. O repositório reúne os quatro módulos que compõem a
pipeline: `raspberry`, `alignment`, `server` e `webApp`.

## Pipeline

1. **Captura — `raspberry/`:** a aplicação recebe uma solicitação de captura,
   obtém imagens das câmeras RGB e NIR e aplica o processamento local.
2. **Alinhamento e NDVI — `raspberry/` com calibração de `alignment/`:** a imagem
   NIR é alinhada à RGB usando a matriz de transformação; em seguida o NDVI é
   calculado, as imagens são recortadas e os arquivos da operação são preparados.
3. **Envio — `raspberry/`:** o uploader envia cada operação ao servidor. Se o
   servidor estiver indisponível, as capturas permanecem na fila local para uma
   nova tentativa.
4. **Armazenamento — `server/`:** a API valida e recebe `rgb.jpg`,
   `metadata.json` e `ndvi.npy`, organizando-os em uma pasta identificada por
   UUID.
5. **Visualização — `webApp/`:** a aplicação web lê as operações armazenadas,
   lista os registros e apresenta imagem RGB, visualização do NDVI e metadados.

## Módulos

### `raspberry/` — captura e processamento embarcado

Aplicação destinada ao Raspberry Pi. `app.py` expõe os endpoints de saúde e
captura; `camera.py` controla as câmeras; `capture.py` orquestra a captura,
processamento e gravação; `processing.py` realiza alinhamento NIR, cálculo de
NDVI e recorte. `uploader.py` envia as operações pendentes para a API em
`server/`, e `trigger.py` é um cliente simples para disparar uma captura.

`config.py` reúne caminhos, parâmetros das câmeras e processamento, além do
endereço do servidor. `alignment.npy` é a matriz de calibração usada pelo
processamento. Os dados temporários e a fila de capturas são gerados em
`raspberry/data/` e não são versionados.

Inicialização da API de captura e do uploader em dois terminais. No primeiro:

```bash
cd raspberry
python3 -m uvicorn app:app --host 0.0.0.0 --port 8000
```

No segundo:

```bash
cd raspberry
python3 uploader.py
```

Para disparar uma captura a partir do Raspberry Pi:

```bash
cd raspberry
python3 trigger.py
```

### `alignment/` — calibração RGB/NIR

Contém `align.py`, imagens de referência e a matriz de alinhamento gerada. O
script estima uma transformação afim entre as imagens RGB e NIR e grava a matriz
informada como argumento. Esta pasta é mantida inteira no repositório para
preservar o código e os materiais da calibração.

```bash
python3 alignment/align.py alignment/rgb.jpg alignment/nir.jpg alignment/alignment.npy
```

O script também gera imagens de preview no diretório de execução.

### `server/` — API de ingestão e armazenamento

API Flask responsável por receber operações e armazenar os três arquivos de
cada captura. O diretório padrão de armazenamento é `entries/` na raiz do
projeto; também pode ser configurado pela variável `ENTRIES_DIR`. Essa pasta é
de dados locais e fica fora do versionamento.

Dependências listadas em `server/requirements.txt`. Para iniciar localmente com
Waitress:

```bash
python3 -m pip install -r server/requirements.txt
cd server
waitress-serve --host=0.0.0.0 --port=5000 app:app
```

### `webApp/` — interface de visualização

Aplicação Next.js que lê as operações armazenadas e apresenta imagens, NDVI e
metadados. Por padrão, procura `entries/` na raiz do projeto; o caminho pode ser
alterado pela variável `ENTRIES_DIR`.

```bash
cd webApp
npm ci
npm run dev
```

Para iniciar uma versão de produção, use `npm run build` e depois `npm start`.

## Conteúdo versionado

O repositório contém somente `alignment/`, `raspberry/`, `server/` e `webApp/`,
além deste README e do `.gitignore`. Pastas auxiliares que existam localmente na
raiz, dependências instaladas, caches, arquivos de ambiente e dados gerados em
execução são ignorados. As matrizes de calibração necessárias à pipeline são
mantidas.
