import { loadData } from "../../../lib/entries";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET(request: Request) {
  const data = await loadData();
  const known = new URL(request.url).searchParams.get("sig");
  if (known && known === data.signature) return Response.json({ unchanged: true, signature: data.signature });
  return Response.json(data, { headers: { "Cache-Control": "no-store" } });
}
