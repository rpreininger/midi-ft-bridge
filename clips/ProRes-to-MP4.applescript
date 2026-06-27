-- ProRes-to-MP4 droplet
-- Drop .mov (or other video) files / folders onto this app.
-- Each is converted to MP4: H.264 video scaled to 288x128, uncompressed PCM audio.
-- Output is written next to the source as <name>.mp4 (or <name>_288x128.mp4 if a .mp4 already exists there).

property targetWidth : 288
property targetHeight : 128
property videoExtensions : {"mov", "mp4", "m4v", "avi", "mkv", "mxf", "prores"}

on findFFmpeg()
	set candidates to {"/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"}
	repeat with c in candidates
		if (do shell script "test -x " & quoted form of (c as text) & " && echo yes || echo no") is "yes" then
			return c as text
		end if
	end repeat
	-- last resort: rely on a login shell PATH
	try
		return do shell script "/bin/bash -lc 'command -v ffmpeg'"
	end try
	error "ffmpeg not found. Install it with: brew install ffmpeg"
end findFFmpeg

on collectFiles(theItems)
	set fileList to {}
	repeat with anItem in theItems
		set p to POSIX path of anItem
		try
			set isDir to (do shell script "test -d " & quoted form of p & " && echo yes || echo no")
		on error
			set isDir to "no"
		end try
		if isDir is "yes" then
			-- gather video files inside the folder (non-recursive, common extensions)
			set found to do shell script "find " & quoted form of p & " -maxdepth 1 -type f \\( -iname '*.mov' -o -iname '*.mp4' -o -iname '*.m4v' -o -iname '*.avi' -o -iname '*.mkv' -o -iname '*.mxf' \\) 2>/dev/null"
			if found is not "" then
				repeat with ln in (paragraphs of found)
					if (ln as text) is not "" then set end of fileList to (ln as text)
				end repeat
			end if
		else
			set end of fileList to p
		end if
	end repeat
	return fileList
end collectFiles

on extensionOf(p)
	try
		return do shell script "f=" & quoted form of p & "; echo \"${f##*.}\" | tr 'A-Z' 'a-z'"
	on error
		return ""
	end try
end extensionOf

on outputPathFor(p)
	-- strip extension, add .mp4; avoid clobbering an existing source mp4
	set base to do shell script "f=" & quoted form of p & "; echo \"${f%.*}\""
	set out to base & ".mp4"
	if (do shell script "test -e " & quoted form of out & " && echo yes || echo no") is "yes" then
		set out to base & "_" & targetWidth & "x" & targetHeight & ".mp4"
	end if
	return out
end outputPathFor

on convertOne(ffmpeg, inPath, outPath)
	set scaleArg to (targetWidth as text) & ":" & (targetHeight as text)
	set cmd to quoted form of ffmpeg & " -y -i " & quoted form of inPath & ¬
		" -vf " & quoted form of ("scale=" & scaleArg) & ¬
		" -c:v libx264 -pix_fmt yuv420p -c:a pcm_s16le -map 0:v:0 -map " & quoted form of "0:a:0?" & " " & ¬
		quoted form of outPath & " -hide_banner -loglevel error"
	do shell script cmd
end convertOne

on run
	display dialog "This is a droplet. Drag video files (or a folder) onto its icon to convert them to MP4 (288x128, PCM audio)." buttons {"OK"} default button "OK" with title "ProRes → MP4"
end run

on open theItems
	set ffmpeg to findFFmpeg()
	set fileList to collectFiles(theItems)

	if (count of fileList) is 0 then
		display dialog "No video files found in the dropped items." buttons {"OK"} default button "OK" with title "ProRes → MP4"
		return
	end if

	set okCount to 0
	set failList to {}

	repeat with inPath in fileList
		set inPathT to inPath as text
		set ext to extensionOf(inPathT)
		if videoExtensions does not contain ext then
			-- skip non-video files silently
		else
			set outPath to outputPathFor(inPathT)
			try
				convertOne(ffmpeg, inPathT, outPath)
				set okCount to okCount + 1
			on error errMsg
				set end of failList to (inPathT & "  —  " & errMsg)
			end try
		end if
	end repeat

	set summary to (okCount as text) & " file(s) converted to MP4 (" & targetWidth & "x" & targetHeight & ", PCM audio)."
	if (count of failList) > 0 then
		set AppleScript's text item delimiters to return
		set summary to summary & return & return & "Failed:" & return & (failList as text)
		set AppleScript's text item delimiters to ""
	end if
	display dialog summary buttons {"OK"} default button "OK" with title "ProRes → MP4"
end open
