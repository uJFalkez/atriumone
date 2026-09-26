import { listEntries } from "../lib/entries";
import Explorer from "./explorer";

export const dynamic = "force-dynamic";

export default async function Page() {
  const data = await listEntries();
  return <Explorer {...data} />;
}
