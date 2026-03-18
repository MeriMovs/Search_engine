package main

import (
	"bytes"
	"encoding/json"
	"flag"
	"fmt"
	"io"
	"log"
	"net/http"
	"strings"
	"time"
)

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

var (
	listenAddr = flag.String("addr", ":8080", "Address to listen on")
	esURL      = flag.String("es-url", "http://localhost:9200", "ElasticSearch base URL")
	esIndex    = flag.String("es-index", "pages", "ElasticSearch index name")
	resultSize = flag.Int("size", 10, "Number of results to return per query")
)

// ---------------------------------------------------------------------------
// ES request / response types
// ---------------------------------------------------------------------------

type esQuery struct {
	Query esMatch `json:"query"`
	Size  int     `json:"size"`
	Source []string `json:"_source"`
}

type esMatch struct {
	Match map[string]string `json:"match"`
}

type esResponse struct {
	Hits struct {
		Total struct {
			Value int `json:"value"`
		} `json:"total"`
		Hits []struct {
			Score  float64 `json:"_score"`
			Source struct {
				URL        string `json:"url"`
				Timestamp  string `json:"timestamp"`
				TokenCount int    `json:"token_count"`
				Text       string `json:"text"`
			} `json:"_source"`
		} `json:"hits"`
	} `json:"hits"`
}

// ---------------------------------------------------------------------------
// Search result returned to the client
// ---------------------------------------------------------------------------

type Result struct {
	URL       string  `json:"url"`
	Timestamp string  `json:"timestamp"`
	Score     float64 `json:"score"`
	Excerpt   string  `json:"excerpt"`
}

type SearchResponse struct {
	Query   string   `json:"query"`
	Total   int      `json:"total"`
	Results []Result `json:"results"`
}

// ---------------------------------------------------------------------------
// ES client
// ---------------------------------------------------------------------------

var httpClient = &http.Client{Timeout: 10 * time.Second}

func search(query string, size int) (*SearchResponse, error) {
	body, err := json.Marshal(esQuery{
		Query:  esMatch{Match: map[string]string{"text": query}},
		Size:   size,
		Source: []string{"url", "timestamp", "token_count", "text"},
	})
	if err != nil {
		return nil, err
	}

	url := fmt.Sprintf("%s/%s/_search", *esURL, *esIndex)
	req, err := http.NewRequest("POST", url, bytes.NewReader(body))
	if err != nil {
		return nil, err
	}
	req.Header.Set("Content-Type", "application/json")

	resp, err := httpClient.Do(req)
	if err != nil {
		return nil, err
	}
	defer resp.Body.Close()

	raw, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("ES returned %d: %s", resp.StatusCode, raw)
	}

	var esResp esResponse
	if err := json.Unmarshal(raw, &esResp); err != nil {
		return nil, err
	}

	results := make([]Result, 0, len(esResp.Hits.Hits))
	for _, h := range esResp.Hits.Hits {
		excerpt := h.Source.Text
		if len(excerpt) > 200 {
			excerpt = excerpt[:200] + "..."
		}
		excerpt = strings.TrimSpace(excerpt)
		results = append(results, Result{
			URL:       h.Source.URL,
			Timestamp: h.Source.Timestamp,
			Score:     h.Score,
			Excerpt:   excerpt,
		})
	}

	return &SearchResponse{
		Query:   query,
		Total:   esResp.Hits.Total.Value,
		Results: results,
	}, nil
}

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

func handleSearch(w http.ResponseWriter, r *http.Request) {
	q := strings.TrimSpace(r.URL.Query().Get("q"))
	if q == "" {
		http.Error(w, `{"error":"missing query parameter 'q'"}`, http.StatusBadRequest)
		return
	}

	size := *resultSize
	if s := r.URL.Query().Get("size"); s != "" {
		fmt.Sscanf(s, "%d", &size)
		if size < 1 || size > 100 {
			size = *resultSize
		}
	}

	resp, err := search(q, size)
	if err != nil {
		log.Printf("search error: %v", err)
		http.Error(w, `{"error":"search failed"}`, http.StatusInternalServerError)
		return
	}

	w.Header().Set("Content-Type", "application/json")
	json.NewEncoder(w).Encode(resp)
}

func handleHealth(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	fmt.Fprintln(w, `{"status":"ok"}`)
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

func main() {
	flag.Parse()

	http.HandleFunc("/search", handleSearch)
	http.HandleFunc("/health", handleHealth)
	http.Handle("/", http.FileServer(http.Dir("static")))

	log.Printf("Search API listening on %s", *listenAddr)
	log.Printf("  ES: %s/%s", *esURL, *esIndex)
	log.Fatal(http.ListenAndServe(*listenAddr, nil))
}
