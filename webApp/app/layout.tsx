import type { Metadata } from "next";
import "./globals.css";

export const metadata: Metadata = {
  title: "AtriumOne",
  description: "Visualizador de operações de sensoriamento remoto da Atrium.",
};

export default function Layout({ children }: { children: React.ReactNode }) {
  return <html lang="pt-BR"><body>{children}</body></html>;
}
