import { readdir, readFile } from "node:fs/promises";
import path from "node:path";

export type Metadata = { [key: string]: string | number | boolean | null | Metadata | unknown[] };
export type Entry = { id: string; metadata: Metadata | null; error?: string };

export const entriesDirectory = path.resolve(process.env.ENTRIES_DIR || "../entries");
const uuid = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;

export function entryFile(id: string, file: string) {
  if (!uuid.test(id)) throw new Error("Identificador inválido.");
  return path.join(entriesDirectory, id, file);
}

export async function listEntries(): Promise<{ entries: Entry[]; error?: string }> {
  try {
    const folders = await readdir(entriesDirectory, { withFileTypes: true });
    const entries = await Promise.all(folders.filter((f) => f.isDirectory() && uuid.test(f.name)).map(async (folder): Promise<Entry> => {
      try {
        const metadata = JSON.parse(await readFile(entryFile(folder.name, "metadata.json"), "utf8"));
        if (!metadata || typeof metadata !== "object" || Array.isArray(metadata)) throw new Error();
        return { id: folder.name, metadata };
      } catch {
        return { id: folder.name, metadata: null, error: "Não foi possível ler metadata.json." };
      }
    }));
    const time = (entry: Entry) => {
      const value = entry.metadata?.timestamp;
      return typeof value === "string" ? Date.parse(value) || 0 : 0;
    };
    entries.sort((a, b) => time(b) - time(a) || a.id.localeCompare(b.id));
    return { entries };
  } catch {
    return { entries: [], error: "Não foi possível acessar a pasta entries. Verifique o caminho e as permissões." };
  }
}
