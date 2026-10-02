"""data/index.html を gzip 圧縮して index.html.gz を作る PlatformIO の pre スクリプト。

ESP32 の Web サーバから 19KB の HTML をそのまま送ると、テザリング経由では
2 秒以上かかりブラウザによってはタイムアウトする。gzip なら 1/4 程度になる。
ファームは index.html.gz があればそちらを Content-Encoding: gzip で返す。
"""

import gzip
import os
import shutil

Import("env")  # noqa: F821  (PlatformIO が注入する)

SRC = os.path.join(env.subst("$PROJECT_DATA_DIR"), "index.html")  # noqa: F821
DST = SRC + ".gz"


def build_gz(*args, **kwargs):
    if not os.path.exists(SRC):
        return
    # 元より新しければ再生成しない
    if os.path.exists(DST) and os.path.getmtime(DST) >= os.path.getmtime(SRC):
        return
    with open(SRC, "rb") as f_in, gzip.GzipFile(DST, "wb", compresslevel=9, mtime=0) as f_out:
        shutil.copyfileobj(f_in, f_out)
    before = os.path.getsize(SRC)
    after = os.path.getsize(DST)
    print(f"gzip_data: index.html {before} -> {after} bytes ({after * 100 // before}%)")


build_gz()
