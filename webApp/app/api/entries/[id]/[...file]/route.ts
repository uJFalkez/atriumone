import { readFile } from "node:fs/promises";
import { entryFile } from "../../../../../lib/entries";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

const files: Record<string, string> = {
  "rgb.jpg": "image/jpeg",
  "nir.jpg": "image/jpeg",
  "metadata.json": "application/json",
  "analysis.json": "application/json",
  "ndvi.npy": "application/octet-stream",
  "preview/rgb.jpg": "image/jpeg",
  "preview/ndvi.png": "image/png",
  "preview/thumb.jpg": "image/jpeg",
};

export async function GET(request: Request, { params }: { params: Promise<{ id: string; file: string[] }> }) {
  const { id, file: parts } = await params;
  const file = parts.join("/");
  if (!Object.hasOwn(files, file)) return new Response("Arquivo desconhecido.", { status: 404 });
  const versioned = new URL(request.url).searchParams.has("v");
  try {
    const data = await readFile(entryFile(id, file));
    const download = new URL(request.url).searchParams.has("download");
    return new Response(new Uint8Array(data), {
      headers: {
        "Content-Type": files[file],
        "Cache-Control": versioned && file !== "metadata.json" ? "private, max-age=31536000, immutable" : "no-store",
        ...(download ? { "Content-Disposition": `attachment; filename="${id.slice(0, 8)}-${file.replace("/", "-")}"` } : {}),
      },
    });
  } catch (error) {
    const missing = (error as NodeJS.ErrnoException).code === "ENOENT";
    return new Response(missing ? "Arquivo não encontrado." : error instanceof Error ? error.message : "Não foi possível abrir o arquivo.", {
      status: missing ? 404 : 422,
      headers: { "Content-Type": "text/plain; charset=utf-8" },
    });
  }
}
