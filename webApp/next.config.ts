import type { NextConfig } from "next";

const nextConfig: NextConfig = {
  // O modo dev (`next dev`) só confia em localhost. A estação é acessada por
  // outros aparelhos da tailnet (ex.: o Mac), então liberamos esses nomes.
  allowedDevOrigins: ["station", "100.109.90.102", "*.ts.net"],
};

export default nextConfig;
