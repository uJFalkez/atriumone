import requests

response = requests.post(
    "http://127.0.0.1:8000/capture",
    timeout=60,
)

response.raise_for_status()

print(response.json())