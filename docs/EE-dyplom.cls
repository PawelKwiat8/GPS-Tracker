\NeedsTeXFormat{LaTeX2e}
\ProvidesClass{EE-dyplom}[2025/01/19 Thesis class]

\LoadClass[12pt, a4paper]{article}

\RequirePackage[utf8]{inputenc}
\RequirePackage[T1]{fontenc}
\RequirePackage{graphicx}
\RequirePackage{geometry}
\RequirePackage{times}

\geometry{
 a4paper,
 total={170mm,257mm},
 left=25mm,
 top=25mm,
}

\def\@instytut{}
\newcommand{\instytut}[1]{\def\@instytut{#1}}

\def\@kierunek{}
\newcommand{\kierunek}[1]{\def\@kierunek{#1}}

\def\@specjalnosc{}
\newcommand{\specjalnosc}[1]{\def\@specjalnosc{#1}}

\def\@album{}
\newcommand{\album}[1]{\def\@album{#1}}

\def\@promotor{}
\newcommand{\promotor}[1]{\def\@promotor{#1}}

\def\@engtitle{}
\newcommand{\engtitle}[1]{\def\@engtitle{#1}}

\renewcommand{\maketitle}{%
  \begin{titlepage}
    \begin{center}
      \vspace*{1cm}
      
      % Logo placeholder - using text if image not found, or standard demo image
      \includegraphics[width=0.4\textwidth]{../images/mel-ikona-wpisu.jpg}
      
      \vspace{1.5cm}
      
      % Removed institute
      
      \vspace{2cm}
      
      {\Huge \bfseries Praca przejściowa \par}
      \vspace{0.5cm}
      {\Huge \bfseries magisterska \par}
      
      \vspace{1.5cm}
      
      
      {\Large \bfseries \@title \par}
      % Removed English title
      
      \vspace{3cm}
      
      {\Large \bfseries \@author \par}
      
      \vspace{2cm}
      
      promotor \par
      \@promotor \par
      
      \vfill
      
      WARSZAWA \@date
      
    \end{center}
  \end{titlepage}
}
