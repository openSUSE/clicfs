SET( DOCS_COMMAND
  COMMAND xsltproc --nonet --xinclude http://docbook.sourceforge.net/release/xsl/current/manpages/docbook.xsl man-clicfs.1.docbook
)

add_custom_target( docs ${DOCS_COMMAND} VERBATIM )

