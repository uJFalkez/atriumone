import type { Metadata, Viewport } from "next";
import "maplibre-gl/dist/maplibre-gl.css";
import "./globals.css";

export const metadata: Metadata = {
  title: "Atrium One · Estação",
  description: "Comando de missões e análise de NDVI do módulo Atrium One.",
};

export const viewport: Viewport = { themeColor: "#0a110f" };

export default function Layout({ children }: { children: React.ReactNode }) {
  return <html lang="pt-BR"><body>{children}</body></html>;
}
