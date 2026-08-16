/* Syntax-highlight shell code blocks in the generated documentation.
 *
 * Doxygen has no shell lexer, so a @code{.sh} block (and a ```bash fence in a
 * Markdown input) comes out as plain black text. Worse, it comes out as markup
 * identical to @code{.text} and @code{.unparsed} - no language class anywhere -
 * so the blocks cannot be told apart from the HTML alone. Guessing is not an
 * option: the unhighlighted blocks on this site also include an ASCII diagram, a
 * C snippet and a couple of file listings, all of which colouring would wreck.
 *
 * So the source marks them instead. Each shell block is preceded by an empty
 *
 *     <div class="lang-sh"></div>
 *
 * (written as @htmlonly[block] in .dox, as plain HTML in .md), and this script
 * highlights the fragment immediately after each marker. Marking is opt-in on
 * purpose: a block someone forgets to mark simply stays plain, whereas an
 * opt-out scheme would silently mangle the first diagram someone forgets.
 *
 * The marker itself is hidden by custom.css, so a reader with JavaScript off
 * sees exactly what they saw before: correct, unhighlighted code.
 */
(function () {
	"use strict";

	/* Shell keywords and builtins only - never arbitrary program names.
	 * Highlighting the first word of every line as "the command" is tempting but
	 * wrong here: one block is captured program output
	 * ("[usbip_device] ctrl type=0x80 ..."), and colouring its first token, or
	 * reading its "type=0x80" as an assignment, would be nonsense. */
	var FLOW = ("if then else elif fi for while until do done case esac in " +
	            "function select break continue").split(" ");
	var BUILTIN = ("sudo export cd source alias unalias echo set unset local " +
	               "readonly declare eval exec exit return shift test trap " +
	               "umask wait pushd popd read").split(" ");
	/* Words after which further NAME=VALUE words on the line are still assignments. */
	var DECLARERS = "export local readonly declare set env".split(" ");

	function setOf(words) {
		var s = {}, i;
		for (i = 0; i < words.length; i++) { s[words[i]] = true; }
		return s;
	}
	var isFlow = setOf(FLOW), isBuiltin = setOf(BUILTIN), isDeclarer = setOf(DECLARERS);

	var IDENT = /^[A-Za-z_][A-Za-z0-9_]*/;
	var ASSIGN = /^([A-Za-z_][A-Za-z0-9_]*)(\+?=)/;
	/* $NAME and ${...}. $(...) is left alone - colouring only its delimiters
	 * reads worse than leaving the substitution plain. */
	var VARIABLE = /^\$(?:\{[^}]*\}|[A-Za-z_][A-Za-z0-9_]*|[0-9?$#@*!-])/;
	var SEPARATORS = ";|&()";

	function esc(s) {
		return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
	}

	function span(cls, text) {
		return '<span class="' + cls + '">' + esc(text) + "</span>";
	}

	function isSpace(ch) {
		return /\s/.test(ch);
	}

	/* Tokenise one shell line into Doxygen-classed HTML. */
	function highlightLine(line) {
		var out = [];
		var i = 0, n = line.length;
		var atCommand = true;   /* next word starts a command */
		var inDeclare = false;  /* an `export`-like word was seen; NAME= still counts */
		var plainStart = 0;     /* start of the pending run of unclassified text */
		var ch, j, m, a, word;

		function flush(upto) {
			if (upto > plainStart) { out.push(esc(line.slice(plainStart, upto))); }
			plainStart = upto;
		}

		/* Emit a quoted run starting at `at`; returns the position after it. */
		function takeString(at) {
			var quote = line.charAt(at), k = at + 1;
			while (k < n) {
				if (quote === '"' && line.charAt(k) === "\\" && k + 1 < n) { k += 2; continue; }
				if (line.charAt(k) === quote) { k += 1; break; }
				k += 1;
			}
			flush(at);
			out.push(span("stringliteral", line.slice(at, k)));
			plainStart = k;
			return k;
		}

		/* Emit a $VAR at `at`; returns the position after it, or -1 if not one. */
		function takeVariable(at) {
			var vm = VARIABLE.exec(line.slice(at));
			if (!vm) { return -1; }
			flush(at);
			out.push(span("preprocessor", vm[0]));
			plainStart = at + vm[0].length;
			return at + vm[0].length;
		}

		/* Consume an assignment's value, so a run of leading NAME=VALUE prefixes
		 * all stay in command position: `A=1 B=2 cmd` assigns both, as a shell
		 * does. Quoted and $-substituted values keep their own colouring. */
		function takeValue(at) {
			var c, k;
			while (at < n && !isSpace(line.charAt(at)) &&
			       SEPARATORS.indexOf(line.charAt(at)) === -1) {
				c = line.charAt(at);
				if (c === "'" || c === '"') { at = takeString(at); continue; }
				if (c === "$") {
					k = takeVariable(at);
					if (k !== -1) { at = k; continue; }
				}
				at += 1;
			}
			return at;
		}

		while (i < n) {
			ch = line.charAt(i);

			/* Comment: '#' at the start of a word only, so a URL like foo#bar and a
			 * bare '#' inside a word stay literal - which is what a shell does too. */
			if (ch === "#" && (i === 0 || isSpace(line.charAt(i - 1)))) {
				flush(i);
				out.push(span("comment", line.slice(i)));
				plainStart = n;
				break;
			}

			/* Quoted strings. */
			if (ch === "'" || ch === '"') {
				i = takeString(i);
				atCommand = false;
				continue;
			}

			/* Variable reference. */
			if (ch === "$") {
				j = takeVariable(i);
				if (j !== -1) {
					i = j;
					atCommand = false;
					continue;
				}
			}

			/* Separators put us back at a command position. */
			if (SEPARATORS.indexOf(ch) !== -1) {
				atCommand = true;
				inDeclare = false;
				i += 1;
				continue;
			}

			if (isSpace(ch)) { i += 1; continue; }

			/* A word. Only words in command position are candidates for keyword or
			 * assignment treatment; everything else is an argument and stays plain. */
			m = IDENT.exec(line.slice(i));
			if (m && (atCommand || inDeclare)) {
				a = ASSIGN.exec(line.slice(i));
				if (a) {
					flush(i);
					out.push(span("preprocessor", a[1]) + esc(a[2]));
					plainStart = i + a[0].length;
					i = takeValue(i + a[0].length);
					/* A leading assignment prefixes a command; another may follow,
					 * so stay in command position. */
					continue;
				}
				word = m[0];
				if (atCommand && (isFlow[word] || isBuiltin[word])) {
					flush(i);
					out.push(span(isFlow[word] ? "keywordflow" : "keyword", word));
					plainStart = i + word.length;
					i += word.length;
					if (isDeclarer[word]) { inDeclare = true; }
					/* `sudo cmd`, `if cmd` - what follows is still a command. */
					continue;
				}
			}

			/* Ordinary word: consume it so its interior is not re-tested. */
			j = i;
			while (j < n && !isSpace(line.charAt(j)) && SEPARATORS.indexOf(line.charAt(j)) === -1) {
				if ("'\"$#".indexOf(line.charAt(j)) !== -1) { break; }
				j += 1;
			}
			i = Math.max(j, i + 1);
			atCommand = false;
		}

		flush(n);
		return out.join("");
	}

	function highlightFragment(fragment) {
		if (fragment.getAttribute("data-sh-highlighted")) { return; }
		fragment.setAttribute("data-sh-highlighted", "1");
		var lines = fragment.querySelectorAll("div.line");
		for (var i = 0; i < lines.length; i++) {
			/* Only ever rewrite a line that is pure text. If it already holds
			 * elements, something else highlighted or linked it - leave it be. */
			if (lines[i].children.length === 0) {
				lines[i].innerHTML = highlightLine(lines[i].textContent);
			}
		}
	}

	function run() {
		var markers = document.querySelectorAll("div.lang-sh");
		for (var i = 0; i < markers.length; i++) {
			var next = markers[i].nextElementSibling;
			/* Deliberately only the immediate sibling: if the marker has drifted
			 * away from its block, do nothing rather than colour the wrong one. */
			if (next && next.className && next.className.indexOf("fragment") !== -1) {
				highlightFragment(next);
			}
		}
	}

	if (document.readyState === "loading") {
		document.addEventListener("DOMContentLoaded", run);
	} else {
		run();
	}
})();
