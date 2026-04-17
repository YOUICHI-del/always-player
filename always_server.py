@app.route("/api/jacket")
def api_jacket():
    try:
        # =========================
        # 再生中ファイル取得
        # =========================
        fp = player.playlist[player.current_index]
        file_path = Path(fp)
        suffix = file_path.suffix.lower()

        # =========================
        # 🔥 FLAC（最優先）
        # =========================
        if suffix == ".flac":
            from mutagen.flac import FLAC
            try:
                audio_flac = FLAC(file_path)

                # ① Picture（通常）
                if audio_flac.pictures:
                    cover = next(
                        (p for p in audio_flac.pictures if p.type == 3),
                        audio_flac.pictures[0]
                    )
                    return send_image_data(cover.data, cover.mime or "image/jpeg")

            except:
                pass

            # ② ID3フォールバック（Mp3tag対策）
            try:
                from mutagen.id3 import ID3
                id3_tags = ID3(file_path)
                apic_keys = [k for k in id3_tags.keys() if k.startswith("APIC")]
                if apic_keys:
                    apic = id3_tags[apic_keys[0]]
                    return send_image_data(apic.data, apic.mime)
            except:
                pass

        # =========================
        # その他形式
        # =========================
        from mutagen import File
        audio = File(fp)

        # -------------------------
        # MP3（ID3）
        # -------------------------
        try:
            from mutagen.id3 import ID3
            tags = ID3(fp)
            apics = tags.getall("APIC")
            if apics:
                front = next((t for t in apics if t.type == 3), apics[0])
                return send_image_data(front.data, front.mime)
        except:
            pass

        # -------------------------
        # M4A / MP4
        # -------------------------
        try:
            from mutagen.mp4 import MP4, MP4Cover
            if isinstance(audio, MP4) and audio.tags and "covr" in audio.tags:
                covers = audio.tags["covr"]
                if covers:
                    data = covers[0]
                    fmt = "image/png" if data.imageformat == MP4Cover.FORMAT_PNG else "image/jpeg"
                    return send_image_data(data, fmt)
        except:
            pass

        # -------------------------
        # WMA（ASF）
        # -------------------------
        try:
            from mutagen.asf import ASF
            if isinstance(audio, ASF):
                pics = audio.get("WM/Picture", [])
                if pics:
                    data = extract_wma_picture(pics[0].value)
                    return send_image_data(data, "image/jpeg")
        except:
            pass

        # -------------------------
        # APE
        # -------------------------
        try:
            from mutagen.apev2 import APEv2
            ape = APEv2(fp)
            if "Cover Art (Front)" in ape:
                data = ape["Cover Art (Front)"].value
                actual_data = data.split(b'\x00', 1)[-1]
                return send_image_data(actual_data, "image/jpeg")
        except:
            pass

        # =========================
        # フォルダ画像 fallback
        # =========================
        try:
            return fallback_folder_image(file_path)
        except:
            pass

        # =========================
        # 🔥 最終（VU止めない）
        # =========================
        return jsonify({
            "ok": True,
            "data": None
        })

    except Exception as e:
        print(f"Jacket error: {e}")
        return jsonify({
            "ok": True,
            "data": None
        })