import type { Station } from "./types";

const EARTH_RADIUS_M = 6_371_000;

// Referencial da missão (metros N/L a partir da estação) -> [lon, lat].
export function toLngLat(station: Station, north: number, east: number): [number, number] {
  const lat = station.latitude + ((north / EARTH_RADIUS_M) * 180) / Math.PI;
  const lon = station.longitude + ((east / (EARTH_RADIUS_M * Math.cos((station.latitude * Math.PI) / 180))) * 180) / Math.PI;
  return [lon, lat];
}
