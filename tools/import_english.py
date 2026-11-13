"""Build the bundled ASCII English dictionary; no runtime download is needed."""
from pathlib import Path
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
WORDS = "https://raw.githubusercontent.com/dwyl/english-words/master/words_alpha.txt"
FREQUENCIES = "https://raw.githubusercontent.com/hermitdave/FrequencyWords/master/content/2018/en/en_full.txt"

def fetch(url):
    with urllib.request.urlopen(url, timeout=60) as response:
        return response.read().decode("utf-8")

def main():
    words = {word.strip(): 0 for word in fetch(WORDS).splitlines() if word.strip().isascii() and word.strip().isalpha()}
    for line in fetch(FREQUENCIES).splitlines():
        word, frequency = line.rsplit(" ", 1)
        if word in words:
            words[word] = int(frequency)
    output = ROOT / "data/english.tsv"
    output.write_text("".join(f"{word}\t{words[word]}\n" for word in sorted(words)), encoding="utf-8")
    print(f"Wrote {len(words):,} English entries to {output}")

if __name__ == "__main__":
    main()
