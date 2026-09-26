import sharp from "sharp";

// NPY stores shape, dtype and memory order in a small Python-dict header.
export async function ndviPng(file: Buffer) {
  if (file.length < 10 || file.subarray(0, 6).toString("hex") !== "934e554d5059") {
    throw new Error("Arquivo NPY inválido.");
  }
  const version = file[6];
  if (![1, 2, 3].includes(version)) throw new Error("Versão NPY não suportada.");
  const start = version === 1 ? 10 : 12;
  if (file.length < start) throw new Error("Cabeçalho NPY incompleto.");
  const length = version === 1 ? file.readUInt16LE(8) : file.readUInt32LE(8);
  const offset = start + length;
  if (offset > file.length) throw new Error("Cabeçalho NPY incompleto.");
  const header = file.subarray(start, offset).toString(version === 3 ? "utf8" : "latin1");
  const dtype = /['"]descr['"]\s*:\s*['"]([^'"]+)['"]/.exec(header)?.[1];
  if (!dtype || !/^[|<>=]?u1$/.test(dtype)) {
    throw new Error(`NDVI deve ser uint8 quantizado; encontrado: ${dtype || "desconhecido"}.`);
  }
  const shape = /['"]shape['"]\s*:\s*\(([^)]*)\)/.exec(header)?.[1]
    .split(",").map((s) => s.trim()).filter(Boolean).map(Number);
  const order = /['"]fortran_order['"]\s*:\s*(True|False)/.exec(header)?.[1];
  if (!shape || shape.length !== 2 || shape.some((n) => !Number.isSafeInteger(n) || n <= 0) || !order) {
    throw new Error("O NDVI precisa ser uma matriz 2D válida.");
  }
  const [height, width] = shape;
  const pixels = height * width;
  if (!Number.isSafeInteger(pixels) || file.length - offset !== pixels) {
    throw new Error("O tamanho do array não corresponde às dimensões do NPY.");
  }
  const output = Buffer.alloc(pixels * 3);
  const red = [220, 65, 62], yellow = [246, 205, 85], green = [38, 153, 98];
  const colors = Array.from({ length: 256 }, (_, q) => {
    const ndvi = q <= 127 ? (q - 127) / 127 : (q - 127) / 128;
    const from = ndvi <= 0 ? red : yellow;
    const to = ndvi <= 0 ? yellow : green;
    const t = ndvi <= 0 ? ndvi + 1 : ndvi;
    return from.map((value, i) => Math.round(value + (to[i] - value) * t));
  });
  for (let i = 0; i < pixels; i++) {
    const index = order === "True" ? (i % width) * height + Math.floor(i / width) : i;
    const color = colors[file[offset + index]];
    output[i * 3] = color[0];
    output[i * 3 + 1] = color[1];
    output[i * 3 + 2] = color[2];
  }
  return sharp(output, { raw: { width, height, channels: 3 } }).png().toBuffer();
}
