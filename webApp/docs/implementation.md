# Visualizador de operações

## Design aprovado

App Next.js contido em `webApp`, lendo `../entries` em tempo de execução.
Sidebar esquerda com operações ordenadas por timestamp decrescente, RGB e NDVI
no centro e metadata à direita. UUID é secundário. Campos null aparecem como
indisponíveis. Interface em português, com os logos de `assets`.

Cada pasta contém `rgb.jpg`, `metadata.json` e `ndvi.npy`. NDVI é uma matriz 2D
uint8 quantizada, com shape e ordem detectados pelo cabeçalho NPY. Conversão:
`q <= 127 ? (q - 127) / 127 : (q - 127) / 128`. Escala fixa vermelho → amarelo →
verde. PNG gerado no servidor, sem Python, banco de dados ou arquivos derivados.
Falhas de uma operação não bloqueiam as demais.

## Plano de implementação

1. `lib/entries.ts`: leitura de pastas, validação dos identificadores, metadata e
   ordenação. `lib/ndvi.ts`: interpretar NPY e gerar PNG com sharp.
2. `app/api/entries/[id]/[file]/route.ts`: servir os três arquivos permitidos e o
   PNG derivado. `app/page.tsx`: carregar entries sem cache persistente.
3. `app/explorer.tsx` e `app/globals.css`: navegação, estados vazio/erro, visualização
   das imagens e grupos de metadata; layout adaptável a telas menores.
4. Instalar dependências, executar build de produção e conferir HTTP com entries
   reais. Não criar suíte de testes, conforme solicitado. Documentar execução.
