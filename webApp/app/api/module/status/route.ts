import { pollModule } from "../../../../lib/module";

export const runtime = "nodejs";
export const dynamic = "force-dynamic";

export async function GET() {
  return Response.json(await pollModule(), { headers: { "Cache-Control": "no-store" } });
}
