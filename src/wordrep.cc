// Author: Karl Stratos (karlstratos@gmail.com)

#include "wordrep.h"

#include <deque>
#include <iomanip>
#include <map>

#include "sparsecca.h"
#include "cluster.h"

void CanonWord::SetOutputDirectory(const string &output_directory) {
    ASSERT(!output_directory.empty(), "Empty output directory.");
    output_directory_ = output_directory;

    // Prepare the output directory and the log file.
    ASSERT(system(("mkdir -p " + output_directory_).c_str()) == 0,
	   "Cannot create directory: " << output_directory_);
    log_.open(LogPath(), ios::out);
    log_ << fixed << setprecision(2);
}

void CanonWord::ResetOutputDirectory() {
    ASSERT(!output_directory_.empty(), "No output directory given.");
    ASSERT(system(("rm -f " + output_directory_ + "/*").c_str()) == 0,
	   "Cannot remove the content in: " << output_directory_);
    log_.close();
    SetOutputDirectory(output_directory_);
}

void CanonWord::ExtractStatistics(const string &corpus_file) {
    FileManipulator file_manipulator;
    if (!file_manipulator.exists(SortedWordTypesPath())) {
	// Only count word types if there is no previous record.
	CountWords(corpus_file);
    }
    DetermineRareWords();
    ComputeCovariance(corpus_file);
}

void CanonWord::InduceLexicalRepresentations() {
    ASSERT(rare_cutoff_ >= 0, "Please specify a value for rare word cutoff.");
    if (word_num2str_.size() == 0 || word_str2num_.size() == 0) {
	// Load the (cutoff-ed) word-integer dictionary if we don't have it.
	LoadWordIntegerDictionary();
    }
    if (wordcount_.size() == 0) {
	// Load the (cutoff-ed) word counts if we don't have them.
	LoadWordCounts();
    }
    // Prepare a list of (cutoff-ed) word types sorted in decreasing frequency.
    vector<pair<string, size_t> > sorted_wordcount(wordcount_.begin(),
						   wordcount_.end());
    sort(sorted_wordcount.begin(), sorted_wordcount.end(),
	 sort_pairs_second<string, size_t, greater<size_t> >());

    // Induce vector representations of word types based on cached count files.
    InduceWordVectors(sorted_wordcount);

    // TODO: Figure out whether to remove k-means entirely.
    // Do K-means clustering over word vectors where K = CCA dimension.
    //PerformKMeans(cca_dim_, sorted_wordcount);
}

Word CanonWord::word_str2num(const string &word_string) {
    ASSERT(word_str2num_.find(word_string) != word_str2num_.end(),
	   "Requesting integer ID of an unknown word string: " << word_string);
    return word_str2num_[word_string];
}

string CanonWord::word_num2str(Word word) {
    ASSERT(word_num2str_.find(word) != word_num2str_.end(),
	   "Requesting string of an unknown word integer: " << word);
    return word_num2str_[word];
}

Context CanonWord::context_str2num(const string &context_string) {
    ASSERT(context_str2num_.find(context_string) != context_str2num_.end(),
	   "Requesting integer ID of an unknown context string: "
	   << context_string);
    return context_str2num_[context_string];
}

string CanonWord::context_num2str(Context context) {
    ASSERT(context_num2str_.find(context) != context_num2str_.end(),
	   "Requesting string of an unknown context integer: " << context);
    return context_num2str_[context];
}

void CanonWord::CountWords(const string &corpus_file) {
    ASSERT(window_size_ >= 2, "Window size less than 2: " << window_size_);
    wordcount_.clear();
    ifstream file(corpus_file, ios::in);
    ASSERT(file.is_open(), "Cannot open file: " << corpus_file);
    string line;
    vector<string> tokens;
    StringManipulator string_manipulator;
    num_words_ = 0;
    while (file.good()) {
	getline(file, line);
	if (line == "") { continue; }
	string_manipulator.split(line, " ", &tokens);
	for (const string &token : tokens) {
	    ASSERT(token != kRareString_, "Rare symbol present: " << token);
	    ASSERT(token != kBufferString_, "Buffer symbol present: " << token);
	    AddWordIfUnknown(token);
	    ++wordcount_[token];
	    ++num_words_;
	}
    }
    ASSERT(num_words_ >= window_size_, "Number of words in the corpus smaller "
	   "than the window size: " << num_words_ << " < " << window_size_);

    // Sort word types in decreasing frequency.
    vector<pair<string, size_t> > sorted_wordcount(wordcount_.begin(),
						   wordcount_.end());
    sort(sorted_wordcount.begin(), sorted_wordcount.end(),
	 sort_pairs_second<string, size_t, greater<size_t> >());

    ofstream sorted_word_types_file(SortedWordTypesPath(), ios::out);
    for (size_t i = 0; i < sorted_wordcount.size(); ++i) {
	string word_string = sorted_wordcount[i].first;
	size_t word_frequency = sorted_wordcount[i].second;
	sorted_word_types_file << word_string << " " << word_frequency << endl;
    }

    // Write a corpus information file.
    ofstream corpus_info_file(CorpusInfoPath(), ios::out);
    corpus_info_file << "Path: " << corpus_file << endl;
    corpus_info_file << num_words_ << " words" << endl;
    corpus_info_file << sorted_wordcount.size() << " word types" << endl;
}

Word CanonWord::AddWordIfUnknown(const string &word_string) {
    ASSERT(!word_string.empty(), "Adding an empty string for word!");
    if (word_str2num_.find(word_string) == word_str2num_.end()) {
	Word word = word_str2num_.size();
	word_str2num_[word_string] = word;
	word_num2str_[word] = word_string;
    }
    return word_str2num_[word_string];
}

Context CanonWord::AddContextIfUnknown(const string &context_string) {
    ASSERT(!context_string.empty(), "Adding an empty string for context!");
    if (context_str2num_.find(context_string) == context_str2num_.end()) {
	Context context = context_str2num_.size();
	context_str2num_[context_string] = context;
	context_num2str_[context] = context_string;
    }
    return context_str2num_[context_string];
}

void CanonWord::DetermineRareWords() {
    // Read in the sorted word type counts.
    string line;
    vector<string> tokens;
    StringManipulator string_manipulator;
    vector<pair<string, size_t> > sorted_wordcount;
    ifstream sorted_word_types_file(SortedWordTypesPath(), ios::in);
    wordcount_.clear();
    word_str2num_.clear();
    word_num2str_.clear();
    num_words_ = 0;
    while (sorted_word_types_file.good()) {
	getline(sorted_word_types_file, line);
	if (line == "") { continue; }
	string_manipulator.split(line, " ", &tokens);
	string word_string = tokens[0];
	size_t word_count = stoi(tokens[1]);
	sorted_wordcount.push_back(make_pair(word_string, word_count));
	AddWordIfUnknown(word_string);
	wordcount_[word_string] += word_count;
	num_words_ += word_count;
    }
    log_ << "[Original corpus]" << endl;
    log_ << "   " << num_words_ << " words" << endl;
    log_ << "   " << sorted_wordcount.size() << " word types" << endl;

    unordered_map<string, bool> rare;  // Keys correspond to rare word types.
    if (rare_cutoff_ >= 0) {
	// If a non-negative cutoff value is specified, use it.
	for (int i = sorted_wordcount.size() - 1; i >= 0; --i) {
	    string word_string = sorted_wordcount[i].first;
	    size_t word_count = sorted_wordcount[i].second;
	    if (word_count > rare_cutoff_) { break; }
	    rare[word_string] = true;
	}
    } else {
	// If no cutoff value is specified, let the model decide some cutoff
	// value (> 0) for determining rare words.
	size_t accumulated_count = 0;
	vector<string> currently_considered_words;
	size_t current_count =
	    sorted_wordcount[sorted_wordcount.size() - 1].second;
	for (int i = sorted_wordcount.size() - 1; i >= 0; --i) {
	    string word_string = sorted_wordcount[i].first;
	    size_t word_count = sorted_wordcount[i].second;
	    if (word_count == current_count) {
		// Collect all words with the same count.
		currently_considered_words.push_back(word_string);
		accumulated_count += word_count;
	    } else {
		// Upon a different word count, add word types collected so far
		// as rare words, and decide whether to continue thresholding.
		rare_cutoff_ = current_count;
		for (string considered_word: currently_considered_words) {
		    rare[considered_word] = true;
		}
		currently_considered_words.clear();
		double rare_mass =
		    ((double) accumulated_count * 100) / num_words_;
		if (rare_mass > 2.0) {
		    // If the rare word mass is more than 5% of the unigram
		    // mass, stop thresholding.
		    break;
		}
		current_count = word_count;
	    }
	}
    }

    double total_count_nonrare = num_words_;
    if (rare.size() > 0) {
	// If we have rare words, record them and remap the word integer IDs.
	ofstream rare_file(RarePath(), ios::out);
	size_t total_count_rare = 0;
	for (const auto &string_pair: rare) {
	    string rare_string = string_pair.first;
	    size_t rare_count = wordcount_[rare_string];
	    rare_file << rare_string << " " << rare_count << endl;
	    total_count_rare += rare_count;
	}
	total_count_nonrare -= total_count_rare;

	unordered_map<string, size_t> wordcount_copy = wordcount_;
	wordcount_.clear();
	word_str2num_.clear();
	word_num2str_.clear();
	AddWordIfUnknown(kRareString_);  // Add a special symbol for rare words.
	for (const auto &word_string_pair: wordcount_copy) {
	    string word_string = word_string_pair.first;
	    size_t word_count = word_string_pair.second;
	    if (rare.find(word_string) != rare.end()) {
		word_string = kRareString_;
	    }
	    AddWordIfUnknown(word_string);
	    wordcount_[word_string] += word_count;
	}
    }

    log_ << endl << "[Processed with cutoff " << rare_cutoff_ << "]" << endl;
    log_ << "   " << rare.size() << " rare word types grouped to "
	 << kRareString_ << endl;
    log_ << "   Now " << wordcount_.size() << " word types" << endl;
    log_ << "   Preserved " << total_count_nonrare / num_words_ * 100
	 << "% of the unigram mass" << endl;

    // Write the word-integer mapping.
    ofstream word_str2num_file(WordStr2NumPath(), ios::out);
    for (const auto &word_pair: word_str2num_) {
	word_str2num_file << word_pair.first << " " << word_pair.second << endl;
    }
}

void CanonWord::ComputeCovariance(const string &corpus_file) {
    // Figure out the indices of the current and context words.
    size_t word_index = (window_size_ % 2 == 0) ?
	(window_size_ / 2) - 1 : window_size_ / 2;  // Right-biased
    vector<size_t> context_indices;
    vector<string> position_markers(window_size_);
    for (size_t context_index = 0; context_index < window_size_;
	 ++context_index) {
	if (context_index != word_index) {
	    context_indices.push_back(context_index);
	    position_markers[context_index] = "w(" +
		to_string(((int) context_index) - ((int) word_index)) + ")=";
	}
    }

    // Put start buffering in the window.
    deque<string> window;
    for (size_t buffering = 0; buffering < word_index; ++buffering) {
	window.push_back(kBufferString_);
    }

    // count_word_context[j][i] = count of word i and context j coocurring
    unordered_map<Word, unordered_map<Word, double> > count_word_context;
    unordered_map<Word, double> count_word;  // i-th: count of word i
    unordered_map<Word, double> count_context;  // j-th: count of context j

    log_ << endl << "[Counting ";
    if (sentence_per_line_) {
	log_ << "(1 line = 1 sentence)";
    } else {
	log_ << "(entire text = 1 sentence)";
    }
    log_ << "]" << endl << flush;
    time_t begin_time_counting = time(NULL);  // Counting time.
    ifstream file(corpus_file, ios::in);
    ASSERT(file.is_open(), "Cannot open file: " << corpus_file);
    string line;
    vector<string> tokens;
    StringManipulator string_manipulator;
    while (file.good()) {
	getline(file, line);
	if (line == "") { continue; }
	string_manipulator.split(line, " ", &tokens);
	for (const string &token : tokens) {
	    string new_string =
		(word_str2num_.find(token) != word_str2num_.end()) ?
		token : kRareString_;
	    window.push_back(new_string);
	    if (window.size() >= window_size_) {
		// Collect statistics from the full window.
		Word word = word_str2num_[window[word_index]];
		++count_word[word];
		for (Word context_index : context_indices) {
		    string context_string = window[context_index];
		    if (!bag_of_words_) {
			context_string =
			    position_markers[context_index] + context_string;
		    }
		    Context context = AddContextIfUnknown(context_string);
		    ++count_context[context];
		    ++count_word_context[context][word];
		}
		window.pop_front();
	    }
	}

	if (sentence_per_line_) {  // End-buffer and collect counts.
	    // But first fill up the window.
	    while (window.size() < window_size_) {
		window.push_back(kBufferString_);
	    }
	    for (size_t buffering = word_index + 1; buffering < window_size_;
		 ++buffering) {
		window.push_back(kBufferString_);
		Word word = word_str2num_[window[word_index]];
		++count_word[word];
		for (Word context_index : context_indices) {
		    string context_string = window[context_index];
		    if (!bag_of_words_) {
			context_string =
			    position_markers[context_index] + context_string;
		    }
		    Context context = AddContextIfUnknown(context_string);
		    ++count_context[context];
		    ++count_word_context[context][word];
		}
		window.pop_front();
	    }
	    window.clear();
	    for (size_t buffering = 0; buffering < word_index; ++buffering) {
		window.push_back(kBufferString_);
	    }
	}
    }

    if (!sentence_per_line_) {  // End-buffer and collect counts.
	// But first fill up the window.
	while (window.size() < window_size_) {
	    window.push_back(kBufferString_);
	}
	for (size_t buffering = word_index + 1; buffering < window_size_;
	     ++buffering) {
	    window.push_back(kBufferString_);
	    Word word = word_str2num_[window[word_index]];
	    ++count_word[word];
	    for (Word context_index : context_indices) {
		string context_string = window[context_index];
		if (!bag_of_words_) {
		    context_string =
			position_markers[context_index] + context_string;
		}
		Context context = AddContextIfUnknown(context_string);
		++count_context[context];
		++count_word_context[context][word];
	    }
	    window.pop_front();
	}
    }
    double time_counting = difftime(time(NULL), begin_time_counting);
    log_ << "   Time taken: " << string_manipulator.print_time(time_counting)
	 << endl;

    // Write the context-integer mapping.
    ofstream context_str2num_file(ContextStr2NumPath(), ios::out);
    for (const auto &context_pair: context_str2num_) {
	context_str2num_file << context_pair.first << " "
			     << context_pair.second << endl;
    }

    // Write the covariance values to an output directory.
    SparseSVDSolver sparsesvd_solver;  // Write a sparse matrix for SVDLIBC.
    sparsesvd_solver.WriteSparseMatrix(count_word_context,
				       CountWordContextPath());

    ofstream count_word_file(CountWordPath(), ios::out);
    for (Word word = 0; word < count_word.size(); ++word) {
	count_word_file << count_word[word] << endl;
    }

    ofstream count_context_file(CountContextPath(), ios::out);
    for (Context context = 0; context < count_context.size(); ++context) {
	count_context_file << count_context[context] << endl;
    }
}

void CanonWord::InduceWordVectors(
    const vector<pair<string, size_t> > &sorted_wordcount) {
    FileManipulator file_manipulator;
    if (!file_manipulator.exists(WordVectorsPath())) {
	// If word vectors are not already computed, perform CCA on word counts.
	Eigen::MatrixXd word_matrix = PerformCCAOnComputedCounts();
	ASSERT(word_matrix.cols() == sorted_wordcount.size(), "CCA projection "
	       "dimension and vocabulary size mismatch: " << word_matrix.cols()
	       << " vs " << sorted_wordcount.size());

	// Write word vectors sorted in decreasing frequency.
	ofstream wordvectors_file(WordVectorsPath(), ios::out);
	for (size_t i = 0; i < sorted_wordcount.size(); ++i) {
	    string word_string = sorted_wordcount.at(i).first;
	    size_t word_count = sorted_wordcount.at(i).second;
	    Word word = word_str2num_[word_string];
	    word_matrix.col(word).normalize();  // Normalize each column (word).
	    wordvectors_file << word_count << " " << word_string;
	    for (size_t j = 0; j < word_matrix.col(word).size(); ++ j) {
		wordvectors_file << " " << word_matrix.col(word)(j);
	    }
	    wordvectors_file << endl;
	}

	// Put word vectors in a PCA basis.
	ChangeOfBasisToPCACoordinates(&word_matrix);

	// Write word vectors in a PCA basis sorted in decreasing frequency.
	ofstream wordvectors_pca_file(WordVectorsPCAPath(), ios::out);
	for (size_t i = 0; i < sorted_wordcount.size(); ++i) {
	    string word_string = sorted_wordcount.at(i).first;
	    size_t word_count = sorted_wordcount.at(i).second;
	    wordvectors_pca_file << word_count << " " << word_string;
	    ASSERT(wordvectors_.find(word_string) != wordvectors_.end(),
		   "No vector computed for the word: " << word_string);
	    for (size_t j = 0; j < wordvectors_[word_string].size(); ++ j) {
		wordvectors_pca_file << " " << wordvectors_[word_string](j);
	    }
	    wordvectors_pca_file << endl;
	}
    } else {
	// Otherwise, load the computed word vectors.
	wordvectors_.clear();
	string line;
	vector<string> tokens;
	StringManipulator string_manipulator;
	ifstream wordvectors_file(WordVectorsPath(), ios::in);
	while (wordvectors_file.good()) {
	    getline(wordvectors_file, line);
	    if (line == "") { continue; }

	    // line = [count] [word_string] [value_{1}] ... [value_{cca_dim_}]
	    string_manipulator.split(line, " ", &tokens);
	    Eigen::VectorXd vector(cca_dim_);
	    for (size_t i = 0; i < cca_dim_; ++i) {
		vector(i) = stod(tokens[i + 2]);
	    }
	    wordvectors_[tokens[1]] = vector;
	}
    }
}

string CanonWord::Signature(size_t version) {
    ASSERT(version <= 2, "Unrecognized signature version: " << version);

    string signature = "cutoff" + to_string(rare_cutoff_);
    if (version >= 1) {
	signature += "_window" + to_string(window_size_);
	if (sentence_per_line_) {
	    signature += "_sentperline";
	}
    }
    if (version >= 2) {
	signature += "_dim" + to_string(cca_dim_);
	signature += "_smooth" + to_string(smoothing_term_);
    }
    return signature;
}

void CanonWord::LoadWordIntegerDictionary() {
    FileManipulator file_manipulator;
    ASSERT(file_manipulator.exists(WordStr2NumPath()), "File not found, "
	   "read from the corpus: " << WordStr2NumPath());

    word_str2num_.clear();
    word_num2str_.clear();
    string line;
    vector<string> tokens;
    StringManipulator string_manipulator;
    ifstream word_str2num_file(WordStr2NumPath(), ios::in);
    while (word_str2num_file.good()) {
	getline(word_str2num_file, line);
	if (line == "") { continue; }
	string_manipulator.split(line, " ", &tokens);
	word_num2str_[stoi(tokens[1])] = tokens[0];
	word_str2num_[tokens[0]] = stoi(tokens[1]);
    }
}

void CanonWord::LoadWordCounts() {
    FileManipulator file_manipulator;
    ASSERT(file_manipulator.exists(SortedWordTypesPath()), "File not found, "
	   "read from the corpus: " << SortedWordTypesPath());
    ASSERT(word_str2num_.size() > 0, "Word-integer dictionary not loaded.");

    ifstream sorted_word_types_file(SortedWordTypesPath(), ios::in);
    string line;
    vector<string> tokens;
    StringManipulator string_manipulator;
    while (sorted_word_types_file.good()) {
	getline(sorted_word_types_file, line);
	if (line == "") { continue; }
	string_manipulator.split(line, " ", &tokens);
	string word_string = tokens[0];
	size_t word_count = stoi(tokens[1]);
	if (word_str2num_.find(word_string) != word_str2num_.end()) {
	    wordcount_[word_string] = word_count;
	} else {
	    wordcount_[kRareString_] += word_count;
	}
    }
}

Eigen::MatrixXd CanonWord::PerformCCAOnComputedCounts() {
    FileManipulator file_manipulator;
    ASSERT(file_manipulator.exists(CountWordContextPath()), "File not found, "
	   "read from the corpus: " << CountWordContextPath());
    ASSERT(file_manipulator.exists(CountWordPath()), "File not found, "
	   "read from the corpus: " << CountWordPath());
    ASSERT(file_manipulator.exists(CountContextPath()), "File not found, "
	   "read from the corpus: " << CountContextPath());

    time_t begin_time_cca = time(NULL);  // CCA time.
    log_ << endl << "[Performing CCA on the computed counts]" << endl;
    string line;
    vector<string> tokens;
    StringManipulator string_manipulator;
    ifstream count_word_context_file(CountWordContextPath(), ios::in);
    getline(count_word_context_file, line);
    string_manipulator.split(line, " ", &tokens);
    log_ << "   Correlation matrix: " << tokens[0] << " x " << tokens[1]
	 << " (" << tokens[2] << " nonzeros)" << endl;
    log_ << "   CCA dimension: " << cca_dim_ << endl;

    if (smoothing_term_ < 0) {
	// If smoothing term is negative, set it based on the cutoff value
	// (i.e., the smallest count) we used.
	smoothing_term_ = rare_cutoff_;
	log_ << "   Smoothing term: " << smoothing_term_ << " (automatically "
	     << "set)" << endl << flush;
    } else {
	log_ << "   Smoothing term: " << smoothing_term_ << endl << flush;
    }

    SparseCCASolver sparsecca_solver(cca_dim_, smoothing_term_);
    sparsecca_solver.PerformCCA(CountWordContextPath(),
				CountWordPath(), CountContextPath());
    double time_cca = difftime(time(NULL), begin_time_cca);
    if (sparsecca_solver.rank() < cca_dim_) {
	log_ << "   (*WARNING*) The correlation matrix has rank "
	     << sparsecca_solver.rank() << " < " << cca_dim_ << "!" << endl;
    }

    singular_values_ = *sparsecca_solver.cca_correlations();
    log_ << "   Condition number: "
	 << singular_values_[0] / singular_values_[cca_dim_ - 1] << endl;
    log_ << "   Time taken: " << string_manipulator.print_time(time_cca)
	 << endl;

    // Write singular values.
    ofstream singular_values_file(SingularValuesPath(), ios::out);
    for (size_t i = 0; i < singular_values_.size(); ++i) {
	singular_values_file << singular_values_[i] << endl;
    }
    return *sparsecca_solver.cca_transformation_x();
}

void CanonWord::ChangeOfBasisToPCACoordinates(Eigen::MatrixXd *word_matrix) {
    time_t begin_time_pca = time(NULL);  // PCA time.
    log_ << endl << "[Change of basis to the PCA coordinates]" << endl << flush;
    for (size_t i = 0; i < word_matrix->rows(); ++i) {
	// Center each dimension (row) to have mean zero.
	double row_mean = word_matrix->row(i).mean();
	for (size_t j = 0; j < word_matrix->cols(); ++j) {
	    (*word_matrix)(i, j) -= row_mean;
	}
    }

    // Do SVD on the centered word matrix and compute right singular vectors.
    Eigen::JacobiSVD<Eigen::MatrixXd> eigen_svd(*word_matrix,
						Eigen::ComputeThinV);
    Eigen::VectorXd singular_values = eigen_svd.singularValues();

    // Compute variance in each dimension.
    ofstream pca_variance_file(PCAVariancePath(), ios::out);
    for (size_t i = 0; i < singular_values.size(); ++i) {
	double ith_variance =
	    pow(singular_values(i), 2) / (word_matrix->cols() - 1);
	pca_variance_file << ith_variance << endl;
    }

    // Compute word vectors in the PCA basis: right singular vectors times
    // scaled by singular values.
    Eigen::MatrixXd word_matrix_pca = eigen_svd.matrixV();
    word_matrix_pca.transposeInPlace();
    for (size_t i = 0; i < word_matrix_pca.rows(); ++i) {
	word_matrix_pca.row(i) *= singular_values(i);
    }
    double time_pca = difftime(time(NULL), begin_time_pca);
    StringManipulator string_manipulator;
    log_ << "   Time taken: " << string_manipulator.print_time(time_pca)
	 << endl;

    // Set word vectors as the columns of word_matrix_pca.
    wordvectors_.clear();
    for (Word word = 0; word < word_num2str_.size(); ++word) {
	string word_string = word_num2str_[word];
	wordvectors_[word_string] = word_matrix_pca.col(word);
    }
}

void CanonWord::PerformKMeans(
    size_t K, const vector<pair<string, size_t> > &sorted_wordcount) {
    ASSERT(wordvectors_.size() > 0, "No word vectors to do K-means on!");

    // Prepare a list of word vectors sorted in decreasing frequency.
    vector<Eigen::VectorXd> sorted_vectors(sorted_wordcount.size());
    for (size_t i = 0; i < sorted_wordcount.size(); ++i) {
	string word_string = sorted_wordcount.at(i).first;
	sorted_vectors[i] = wordvectors_[word_string];
    }

    // Do K-means clustering over word vectors sorted in decreasing frequency
    // where K is the CCA dimension.
    time_t begin_time_kmeans = time(NULL);  // K-means time.
    log_ << endl << "[" << K << "-means clustering with frequent words "
	 << "as initial centroids]" << endl;
    vector<size_t> cluster_mapping;
    KMeansSolver kmeans_solver;
    bool kmeans_converged =
	kmeans_solver.Cluster(sorted_vectors, K, &cluster_mapping, 20);
    double time_kmeans = difftime(time(NULL), begin_time_kmeans);
    StringManipulator string_manipulator;
    if (kmeans_converged) {
	log_ << "   Converged" << endl;
    } else {
	log_ << "   Did not converge" << endl;
    }
    log_ << "   Time taken: " << string_manipulator.print_time(time_kmeans)
	 << endl;

    // Write the clusters.
    unordered_map<size_t, vector<Word> > cluster2word;
    for (Word word = 0; word < sorted_wordcount.size(); ++word) {
	size_t assigned_cluster_num = cluster_mapping[word];
	cluster2word[assigned_cluster_num].push_back(word);
    }
    ofstream kmeans_file(KMeansPath(), ios::out);
    for (const auto &cluster_pair : cluster2word) {
	size_t cluster_num = cluster_pair.first;
	for (Word word : cluster_pair.second) {
	    string word_string = sorted_wordcount[word].first;
	    size_t word_frequency = sorted_wordcount[word].second;
	    kmeans_file << cluster_num << " " << word_string << " "
			<< word_frequency << endl;
	}
    }
}
