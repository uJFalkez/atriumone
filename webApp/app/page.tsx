import { loadData } from "../lib/entries";
import Dashboard from "./components/Dashboard";

export const dynamic = "force-dynamic";

export default async function Page() {
  return <Dashboard initial={await loadData()} />;
}
