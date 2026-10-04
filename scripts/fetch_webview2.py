import os
import sys
import urllib.request
import zipfile
import shutil

PACKAGE_URL = "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2"
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.dirname(SCRIPT_DIR)
THIRD_PARTY_DIR = os.path.join(REPO_ROOT, "third_party", "webview2")
ZIP_PATH = os.path.join(REPO_ROOT, "third_party", "webview2.nupkg")

def main():
    print(f"Target directory: {THIRD_PARTY_DIR}")
    os.makedirs(os.path.join(REPO_ROOT, "third_party"), exist_ok=True)

    print(f"Downloading WebView2 package from {PACKAGE_URL}...")
    headers = {"User-Agent": "VoxelEngineBuild/1.0"}
    req = urllib.request.Request(PACKAGE_URL, headers=headers)
    with urllib.request.urlopen(req) as resp, open(ZIP_PATH, "wb") as out_file:
        shutil.copyfileobj(resp, out_file)

    print("Extracting package...")
    if os.path.exists(THIRD_PARTY_DIR):
        shutil.rmtree(THIRD_PARTY_DIR)
    os.makedirs(THIRD_PARTY_DIR, exist_ok=True)

    with zipfile.ZipFile(ZIP_PATH, "r") as z:
        z.extractall(THIRD_PARTY_DIR)

    if os.path.exists(ZIP_PATH):
        os.remove(ZIP_PATH)

    print("WebView2 package extracted successfully.")
    native_dir = os.path.join(THIRD_PARTY_DIR, "build", "native")
    if os.path.exists(native_dir):
        print(f"Contents of {native_dir}:")
        for root, dirs, files in os.walk(native_dir):
            for f in files:
                rel = os.path.relpath(os.path.join(root, f), THIRD_PARTY_DIR)
                print(f"  {rel}")
    else:
        print("Warning: build/native directory not found in package.")

if __name__ == "__main__":
    main()
