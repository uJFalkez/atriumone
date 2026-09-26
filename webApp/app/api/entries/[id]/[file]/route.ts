import { readFile } from "node:fs/promises";
import { entryFile } from "../../../../../lib/entries";
import { ndviPng } from "../../../../../lib/ndvi";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const files: Record<string, string> = {
  "rgb.jpg": "image/jpeg",
  "metadata.json": "application/json",
  "ndvi.npy": "application/octet-stream",
  "ndvi.png": "image/png",
};

export async function GET(_request: Request, { params }: { params: Promise<{ id: string; file: string }> }) {
  const { id, file } = await params;
  if (!Object.hasOwn(files, file)) return new Response("Arquivo desconhecido.", { status: 404 });
  try {
    const source = await readFile(entryFile(id, file === "ndvi.png" ? "ndvi.npy" : file));
    const data = file === "ndvi.png" ? await ndviPng(source) : source;
    return new Response(new Uint8Array(data), {
      headers: { "Content-Type": files[file], "Cache-Control": "no-store" },
    });
  } catch (error) {
    const missing = (error as NodeJS.ErrnoException).code === "ENOENT";
    return new Response(missing ? "Arquivo não encontrado." : error instanceof Error ? error.message : "Não foi possível abrir o arquivo.", {
      status: missing ? 404 : 422,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
}
