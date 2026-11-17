# Artigo AP2

No projeto SBrT da disciplina, mantenha o arquivo de classe `sbrt.cls`, carregue o conteúdo desta pasta, selecione `main.tex` como documento principal e recompile. O estilo bibliográfico é IEEEtran, com BibTeX. Em uma instalação TeX completa, o ciclo é:

```sh
pdflatex main
bibtex main
pdflatex main
pdflatex main
```

As dez referências externas e a entrada do repositório são mantidas em [refs.bib](refs.bib). A entrada adicional do tipo `IEEEtranBSTCTL` configura a abreviação de listas de autores. As figuras são PDFs vetoriais. A foto da montagem está no README principal.
