import { readFile } from "node:fs/promises";
import path from "node:path";

export async function GET() {
  const icon = await readFile(path.join(process.cwd(), "assets/favicon.ico"));
  return new Response(icon, { headers: { "Content-Type": "image/x-icon" } });
}
