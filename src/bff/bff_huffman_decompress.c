/*-
 * Copyright (c) 2009 Xin LI <delphij@FreeBSD.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

// compile it using: gcc bff_huffman_decompress.c -o bff_huffman_decompress
// use it as: ./bff_huffman_decompress sampleBff_file0 sampleBff_file0_parsed 

/*
 * pack(1) file format:
 *
 * The first byte is the header:
 *	    00 - Level for the huffman tree (<=24)
 *
 * pack(1) will then store symbols (leaf) nodes count in each huffman
 * tree levels, each level would consume 1 byte (See [1]).
 *
 * After the symbol count table, there is the symbol table, storing
 * symbols represented by corresponding leaf node.  EOB is not being
 * explicitly transmitted (not necessary anyway) in the symbol table.
 *
 * Compressed data goes after the symbol table.
 *
 * NOTES
 *
 * [1] If we count EOB into the symbols, that would mean that we will
 * have at most 256 symbols in the huffman tree.  pack(1) rejects empty
 * file and files that just repeats one character, which means that we
 * will have at least 2 symbols.  Therefore, pack(1) would reduce the
 * last level symbol count by 2 which makes it a number in
 * range [0..254], so all levels' symbol count would fit into 1 byte.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define	PACK_HEADER_LENGTH	1
#define	HTREE_MAXLEVEL		24

/*
 * unpack descriptor
 *
 * Represent the huffman tree in a similar way that pack(1) would
 * store in a packed file.  We store all symbols in a linear table,
 * and store pointers to each level's first symbol.  In addition to
 * that, maintain two counts for each level: inner nodes count and
 * leaf nodes count.
 */
typedef struct {
	int		symbol_size;	/* Size of the symbol table */
	int		treelevels;	/* Levels for the huffman tree */

	int		*symbolsin;	/* Table of leaf symbols count in
					   each level */
	int		*inodesin;	/* Table of internal nodes count in
					   each level */

	char		*symbol;	/* The symbol table */
	char		*symbol_eob;	/* Pointer to the EOB symbol */
	char		**tree;		/* Decoding huffman tree (pointers to
					   first symbol of each tree level */

	off_t		uncompressed_size; /* Uncompressed size */
	FILE		*fpIn;		/* Input stream */
	FILE		*fpOut;		/* Output stream */
} unpack_descriptor_t;

int maybe_err(char*fmt,...) {

}
int maybe_errx(char*fmt,...) {

}

/*
 * Release resource allocated to an unpack descriptor.
 *
 * Caller is responsible to make sure that all of these pointers are
 * initialized (in our case, they all point to valid memory block).
 * We don't zero out pointers here because nobody else would ever
 * reference the memory block without scrubbing them.
 */
static void
unpack_descriptor_fini(unpack_descriptor_t *unpackd)
{

	free(unpackd->symbolsin);
	free(unpackd->inodesin);
	free(unpackd->symbol);
	free(unpackd->tree);

	fclose(unpackd->fpIn);
	fclose(unpackd->fpOut);
}

/*
 * Recursively fill the internal node count table
 */
static void
unpackd_fill_inodesin(const unpack_descriptor_t *unpackd, int level)
{

	/*
	 * The internal nodes would be 1/2 of total internal nodes and
	 * leaf nodes in the next level.  For the last level there
	 * would be no internal node by definition.
	 */
	if (level < unpackd->treelevels) {
		unpackd_fill_inodesin(unpackd, level + 1);
		unpackd->inodesin[level] = (unpackd->inodesin[level + 1] +
					  unpackd->symbolsin[level + 1]) / 2;
	} else
		unpackd->inodesin[level] = 0;
}

/*
 * Update counter for accepted bytes
 */
static void
accepted_bytes(off_t *bytes_in, off_t newbytes)
{

	if (bytes_in != NULL)
		(*bytes_in) += newbytes;
}

/*
 * Read file header and construct the tree.  Also, prepare the buffered I/O
 * for decode routine.
 *
 * Return value is uncompressed size.
 */
static void
unpack_parse_header(int in, int out, char *pre, size_t prelen, off_t *bytes_in,
    unpack_descriptor_t *unpackd)
{
	unsigned char hdr[PACK_HEADER_LENGTH];	/* buffer for header */
	ssize_t bytesread;		/* Bytes read from the file */
	int i, j, thisbyte;

	/* Prepend the header buffer if we already read some data */
	if (prelen != 0)
		memcpy(hdr, pre, prelen);

	/* Read in and fill the rest bytes of header */
	bytesread = read(in, hdr + prelen, PACK_HEADER_LENGTH - prelen);
	if (bytesread < 0)
		maybe_err("Error reading pack header");

	accepted_bytes(bytes_in, PACK_HEADER_LENGTH);

	/* Reset uncompressed size */
	unpackd->uncompressed_size = 0;

	/* Get the levels of the tree */
	unpackd->treelevels = hdr[0];
	if (unpackd->treelevels > HTREE_MAXLEVEL || unpackd->treelevels < 1)
		maybe_errx("Huffman tree has insane levels");

	/* Let libc take care for buffering from now on */
	if ((unpackd->fpIn = fdopen(in, "r")) == NULL)
		maybe_err("Can not fdopen() input stream");
	if ((unpackd->fpOut = fdopen(out, "w")) == NULL)
		maybe_err("Can not fdopen() output stream");

	/* Allocate for the tables of bounds and the tree itself */
	unpackd->inodesin =
	    calloc(unpackd->treelevels, sizeof(*(unpackd->inodesin)));
	unpackd->symbolsin =
	    calloc(unpackd->treelevels, sizeof(*(unpackd->symbolsin)));
	unpackd->tree =
	    calloc(unpackd->treelevels, (sizeof (*(unpackd->tree))));
	if (unpackd->inodesin == NULL || unpackd->symbolsin == NULL ||
	    unpackd->tree == NULL)
		maybe_err("calloc");

	/* We count from 0 so adjust to match array upper bound */
	unpackd->treelevels--;

	/* Read the levels symbol count table and calculate total */
	unpackd->symbol_size = 1;		/* EOB */
	for (i = 0; i <= unpackd->treelevels; i++) {
		if ((thisbyte = fgetc(unpackd->fpIn)) == EOF)
			maybe_err("File appears to be truncated");
		unpackd->symbolsin[i] = (unsigned char)thisbyte;
		unpackd->symbol_size += unpackd->symbolsin[i];
	}
	accepted_bytes(bytes_in, unpackd->treelevels);
	if (unpackd->symbol_size > 256)
		maybe_errx("Bad symbol table");

	/* Allocate for the symbol table, point symbol_eob at the beginning */
	unpackd->symbol_eob = unpackd->symbol = calloc(1, unpackd->symbol_size);
	if (unpackd->symbol == NULL)
		maybe_err("calloc");

	/*
	 * Read in the symbol table, which contain [2, 256] symbols.
	 * In order to fit the count in one byte, pack(1) would offset
	 * it by reducing 2 from the actual number from the last level.
	 *
	 * We adjust the last level's symbol count by 1 here, because
	 * the EOB symbol is not being transmitted explicitly.  Another
	 * adjustment would be done later afterward.
	 */
	unpackd->symbolsin[unpackd->treelevels]++;
	for (i = 0; i <= unpackd->treelevels; i++) {
		unpackd->tree[i] = unpackd->symbol_eob;
		for (j = 0; j < unpackd->symbolsin[i]; j++) {
			if ((thisbyte = fgetc(unpackd->fpIn)) == EOF)
				maybe_errx("Symbol table truncated");
			*unpackd->symbol_eob++ = (char)thisbyte;
		}
		accepted_bytes(bytes_in, unpackd->symbolsin[i]);
	}

	/* Now, take account for the EOB symbol as well */
	unpackd->symbolsin[unpackd->treelevels]++;

	/*
	 * The symbolsin table has been constructed now.
	 * Calculate the internal nodes count table based on it.
	 */
	unpackd_fill_inodesin(unpackd, 0);
}

/*
 * Decode huffman stream, based on the huffman tree.
 */
static void
unpack_decode(unpack_descriptor_t *unpackd, off_t *bytes_in)
{
	int thislevel, thiscode, thisbyte, inlevelindex;
	int i;
	off_t bytes_out = 0;
	const char *thissymbol;	/* The symbol pointer decoded from stream */

	/*
	 * Decode huffman.  Fetch every bytes from the file, get it
	 * into 'thiscode' bit-by-bit, then output the symbol we got
	 * when one has been found.
	 *
	 * Assumption: sizeof(int) > ((max tree levels + 1) / 8).
	 * bad things could happen if not.
	 */
	thislevel = 0;
	thiscode = thisbyte = 0;

	while ((thisbyte = fgetc(unpackd->fpIn)) != EOF) {
		accepted_bytes(bytes_in, 1);

		/*
		 * Split one bit from thisbyte, from highest to lowest,
		 * feed the bit into thiscode, until we got a symbol from
		 * the tree.
		 */
		for (i = 7; i >= 0; i--) {
			thiscode = (thiscode << 1) | ((thisbyte >> i) & 1);

			/* Did we got a symbol? (referencing leaf node) */
			if (thiscode >= unpackd->inodesin[thislevel]) {
				inlevelindex =
				    thiscode - unpackd->inodesin[thislevel];
				if (inlevelindex > unpackd->symbolsin[thislevel])
					maybe_errx("File corrupt");

				thissymbol =
				    &(unpackd->tree[thislevel][inlevelindex]);
				if (thissymbol == unpackd->symbol_eob)
					goto finished;

				fputc((*thissymbol), unpackd->fpOut);
				bytes_out++;

				/* Prepare for next input */
				thislevel = 0; thiscode = 0;
			} else {
				thislevel++;
				if (thislevel > unpackd->treelevels)
					maybe_errx("File corrupt");
			}
		}
	}

finished:
	if (bytes_out != unpackd->uncompressed_size)
		maybe_errx("Premature EOF");
    unpackd->uncompressed_size=bytes_out;  // hack
}

/* Handler for pack(1)'ed file */
static off_t
unpack(int in, int out, char *pre, size_t prelen, off_t *bytes_in)
{
	unpack_descriptor_t	unpackd;

	unpack_parse_header(dup(in), dup(out), pre, prelen, bytes_in, &unpackd);
	unpack_decode(&unpackd, bytes_in);
	unpack_descriptor_fini(&unpackd);

	/* If we reached here, the unpack was successful */
	return (unpackd.uncompressed_size);
}
void
usage() {
    printf("Usage:\n    ./bff_huffman_decompress INFILE OUTFILE\n");
}
int
main(int argc,char**argv) {
    if (argc<3) {
        fprintf(stderr,"[!] Please specify the input and output file as command line arguments\n");
        usage();
        return 1;
    }
    FILE*in=fopen(argv[1],"r");
    if (!in) {
        fprintf(stderr,"[-] Could *not* open input file\n");
        return 1;
    }
    int in_fd=fileno(in);
    FILE*out=fopen(argv[2],"w");
    if (!out) {
        fprintf(stderr,"[-] Could *not* open output file\n");
        return 1;
    }
    int out_fd=fileno(out);
    off_t uncompressed_size=unpack(in_fd,out_fd,0,0,0);
    if (uncompressed_size>0) {
        printf("[+] File was successfully decompressed, decompressed size is %lu (%luKB)\n",
            uncompressed_size,uncompressed_size/1024);
        return 0;
    } else {
        fprintf(stderr,"[-] Decompression of the file *not* succeeded. FAILED!\n");
        return 1;
    }
    return 0;
}

