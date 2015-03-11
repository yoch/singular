// Author: Karl Stratos (stratos@cs.columbia.edu)

#include "wordrep.h"

#include <dirent.h>
#include <iomanip>
#include <limits>
#include <map>

#include "cluster.h"
#include "sparsesvd.h"
#include "wsqloss.h"

void WordRep::SetOutputDirectory(const string &output_directory) {
    ASSERT(!output_directory.empty(), "Empty output directory.");
    output_directory_ = output_directory;

    // Prepare the output directory and the log file.
    ASSERT(system(("mkdir -p " + output_directory_).c_str()) == 0,
	   "Cannot create directory: " << output_directory_);

    // Obtain the latest version marker for log.
    size_t latest_version = 0;
    DIR *directory = opendir(output_directory_.c_str());
    struct dirent *entry;
    while (NULL != (entry = readdir(directory))) {
	string file_name = entry->d_name;
	if (file_name.substr(0, 3) == "log") {
	    size_t version = stol(file_name.substr(4));
	    if (version > latest_version) { latest_version = version; }
	}
    }
    closedir(directory);
    log_.open(LogPath() + "." + to_string(latest_version + 1), ios::out);
    log_ << fixed << setprecision(3);
}

void WordRep::ResetOutputDirectory() {
    ASSERT(!output_directory_.empty(), "No output directory given.");
    ASSERT(system(("rm -f " + output_directory_ + "/*").c_str()) == 0,
	   "Cannot remove the content in: " << output_directory_);
    log_.close();
    SetOutputDirectory(output_directory_);
}

void WordRep::ExtractStatistics(const string &corpus_file) {
    CountWords(corpus_file);
    DetermineRareWords();
    SlideWindow(corpus_file);
}

void WordRep::InduceLexicalRepresentations() {
    // Load a filtered word dictionary from a cached file.
    LoadWordDictionary();

    // Load a sorted list of word-count pairs from a cached file.
    LoadSortedWordCounts();

    // Induce word vectors from cached count files.
    InduceWordVectors();

    // Test the quality of word vectors on simple tasks.
    TestQualityOfWordVectors();

    // Perform greedy agglomerative clustering over word vectors.
    PerformAgglomerativeClustering(dim_);
}

Word WordRep::word_str2num(const string &word_string) {
    ASSERT(word_str2num_.find(word_string) != word_str2num_.end(),
	   "Requesting integer ID of an unknown word string: " << word_string);
    return word_str2num_[word_string];
}

string WordRep::word_num2str(Word word) {
    ASSERT(word_num2str_.find(word) != word_num2str_.end(),
	   "Requesting string of an unknown word integer: " << word);
    return word_num2str_[word];
}

Context WordRep::context_str2num(const string &context_string) {
    ASSERT(context_str2num_.find(context_string) != context_str2num_.end(),
	   "Requesting integer ID of an unknown context string: "
	   << context_string);
    return context_str2num_[context_string];
}

string WordRep::context_num2str(Context context) {
    ASSERT(context_num2str_.find(context) != context_num2str_.end(),
	   "Requesting string of an unknown context integer: " << context);
    return context_num2str_[context];
}

void WordRep::CountWords(const string &corpus_file) {
    FileManipulator file_manipulator;  // Do not repeat the work.
    if (file_manipulator.Exists(SortedWordTypesPath())) { return; }

    ASSERT(window_size_ >= 2, "Window size less than 2: " << window_size_);
    ifstream file(corpus_file, ios::in);
    ASSERT(file.is_open(), "Cannot open file: " << corpus_file);
    string line;
    vector<string> tokens;
    StringManipulator string_manipulator;
    unordered_map<string, size_t> wordcount;
    size_t num_words = 0;
    while (file.good()) {
	getline(file, line);
	if (line == "") { continue; }
	string_manipulator.Split(line, " ", &tokens);
	for (const string &token : tokens) {
	    ASSERT(token != kRareString_, "Rare symbol present: " << token);
	    ASSERT(token != kBufferString_, "Buffer symbol present: " << token);
	    AddWordIfUnknown(token);
	    ++wordcount[token];
	    ++num_words;
	}
    }
    ASSERT(num_words >= window_size_, "Number of words in the corpus smaller "
	   "than the window size: " << num_words << " < " << window_size_);

    // Sort word types in decreasing frequency.
    vector<pair<string, size_t> > sorted_wordcount(wordcount.begin(),
						   wordcount.end());
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
    corpus_info_file << num_words << " words" << endl;
    corpus_info_file << sorted_wordcount.size() << " word types" << endl;
}

Word WordRep::AddWordIfUnknown(const string &word_string) {
    ASSERT(!word_string.empty(), "Adding an empty string for word!");
    if (word_str2num_.find(word_string) == word_str2num_.end()) {
	Word word = word_str2num_.size();
	word_str2num_[word_string] = word;
	word_num2str_[word] = word_string;
    }
    return word_str2num_[word_string];
}

void WordRep::DetermineRareWords() {
    StringManipulator string_manipulator;
    string line;
    vector<string> tokens;

    // Read in the original word dictionary and sorted word type counts.
    word_str2num_.clear();
    word_num2str_.clear();
    size_t num_words = 0;
    vector<pair<string, size_t> > sorted_wordcount;
    ifstream sorted_word_types_file(SortedWordTypesPath(), ios::in);
    while (sorted_word_types_file.good()) {
	getline(sorted_word_types_file, line);
	if (line == "") { continue; }
	string_manipulator.Split(line, " ", &tokens);
	string word_string = tokens[0];
	size_t word_count = stol(tokens[1]);
	sorted_wordcount.push_back(make_pair(word_string, word_count));
	AddWordIfUnknown(word_string);
	num_words += word_count;
    }
    log_ << "[Corpus]" << endl;
    log_ << "   Number of words: " << num_words << endl;
    log_ << "   Cutoff: " << rare_cutoff_ << ", " << word_str2num_.size()
	 << " => ";

    // Filter the dictionary by grouping rare types into a single type.
    unordered_map<string, bool> rare;
    size_t num_rare = 0;
    ofstream rare_file(RarePath(), ios::out);
    word_str2num_.clear();
    word_num2str_.clear();
    for (int i = sorted_wordcount.size() - 1; i >= 0; --i) {
	string word_string = sorted_wordcount[i].first;
	size_t word_count = sorted_wordcount[i].second;
	if (word_count <= rare_cutoff_) {
	    rare[word_string] = true;
	    num_rare += word_count;
	    rare_file << word_string << " " << word_count << endl;
	    AddWordIfUnknown(kRareString_);
	} else {
	    AddWordIfUnknown(word_string);
	}
    }
    double preserved_unigram_mass =
	((double) num_words - num_rare) / num_words * 100;

    log_ << word_str2num_.size() << " word types" << endl;
    log_ << "   Uncut: " << preserved_unigram_mass
	 << "% unigram mass" << endl;

    // Write the filtered word dictionary.
    ofstream word_str2num_file(WordStr2NumPath(), ios::out);
    for (const auto &word_pair: word_str2num_) {
	word_str2num_file << word_pair.first << " " << word_pair.second << endl;
    }
}

void WordRep::SlideWindow(const string &corpus_file) {
    string corpus_format = (sentence_per_line_) ? "1 line = 1 sentence" :
	"Whole Text = 1 sentence";
    log_ << endl << "[Sliding window]" << endl;
    log_ << "   Window size: " << window_size_ << endl;
    log_ << "   Context definition: " << context_definition_ << endl;
    log_ << "   Corpus format: " << corpus_format << endl << flush;

    // If we already have count files, do not repeat the work.
    FileManipulator file_manipulator;
    if (file_manipulator.Exists(ContextStr2NumPath()) &&
	file_manipulator.Exists(CountWordContextPath()) &&
	file_manipulator.Exists(CountWordPath()) &&
	file_manipulator.Exists(CountContextPath())) {
	log_ << "   Counts already exist" << endl;
	return;
    }

    // Pre-compute values we need over and over again.
    size_t word_index = (window_size_ - 1) / 2;  // Right-biased
    vector<string> position_markers(window_size_);
    for (size_t context_index = 0; context_index < window_size_;
	 ++context_index) {
	if (context_index != word_index) {
	    position_markers[context_index] = "w(" +
		to_string(((int) context_index) - ((int) word_index)) + ")=";
	}
    }

    // count_word_context[j][i] = count of word i and context j coocurring
    unordered_map<Context, unordered_map<Word, double> > count_word_context;
    unordered_map<Word, double> count_word;  // i-th: count of word i
    unordered_map<Context, double> count_context;  // j-th: count of context j

    // Put start buffering in the window.
    deque<string> window;
    for (size_t buffering = 0; buffering < word_index; ++buffering) {
	window.push_back(kBufferString_);
    }

    time_t begin_time_sliding = time(NULL);  // Window sliding time.
    ifstream file(corpus_file, ios::in);
    ASSERT(file.is_open(), "Cannot open file: " << corpus_file);
    StringManipulator string_manipulator;
    string line;
    vector<string> tokens;
    while (file.good()) {
	getline(file, line);
	if (line == "") { continue; }
	string_manipulator.Split(line, " ", &tokens);
	for (const string &token : tokens) {
	    // TODO: Switch to checking rare dictionary?
	    string new_string =
		(word_str2num_.find(token) != word_str2num_.end()) ?
		token : kRareString_;
	    window.push_back(new_string);
	    if (window.size() >= window_size_) {  // Full window.
		ProcessWindow(window, word_index, position_markers,
			      &count_word, &count_context, &count_word_context);
		window.pop_front();
	    }
	}

	if (sentence_per_line_) {
	    FinishWindow(word_index, position_markers, &window,
			 &count_word, &count_context, &count_word_context);
	}
    }

    if (!sentence_per_line_) {
	FinishWindow(word_index, position_markers, &window,
		     &count_word, &count_context, &count_word_context);
    }

    double time_sliding = difftime(time(NULL), begin_time_sliding);
    log_ << "   Time taken: " << string_manipulator.TimeString(time_sliding)
	 << endl;

    // Write the filtered context dictionary.
    ofstream context_str2num_file(ContextStr2NumPath(), ios::out);
    for (const auto &context_pair: context_str2num_) {
	context_str2num_file << context_pair.first << " "
			     << context_pair.second << endl;
    }

    // Write counts to the output directory.
    SparseSVDSolver sparsesvd_solver;  // Write as a sparse matrix for SVDLIBC.
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

void WordRep::FinishWindow(size_t word_index,
			   const vector<string> &position_markers,
			   deque<string> *window,
			   unordered_map<Word, double> *count_word,
			   unordered_map<Context, double> *count_context,
			   unordered_map<Context, unordered_map<Word, double> >
			   *count_word_context) {
    size_t original_window_size = window->size();
    while (window->size() < window_size_) {
	// First fill up the window in case the sentence was short.
	(*window).push_back(kBufferString_);  //   [<!> a] -> [<!> a <!> ]
    }
    for (size_t buffering = word_index; buffering < original_window_size;
	 ++buffering) {
	ProcessWindow(*window, word_index, position_markers,
		      count_word, count_context, count_word_context);
	(*window).pop_front();
	(*window).push_back(kBufferString_);
    }
    (*window).clear();
    for (size_t buffering = 0; buffering < word_index; ++buffering) {
	(*window).push_back(kBufferString_);
    }
}

void WordRep::ProcessWindow(const deque<string> &window,
			    size_t word_index,
			    const vector<string> &position_markers,
			    unordered_map<Word, double> *count_word,
			    unordered_map<Context, double> *count_context,
			    unordered_map<Context, unordered_map<Word, double> >
			    *count_word_context) {
    Word word = word_str2num_[window.at(word_index)];
    (*count_word)[word] += 1;

    for (size_t context_index = 0; context_index < window.size();
	 ++context_index) {
	if (context_index == word_index) { continue; }
	string context_string = window.at(context_index);
	if (context_definition_ == "bag") {  // Bag-of-words (BOW).
	    Context bag_context = AddContextIfUnknown(context_string);
	    (*count_context)[bag_context] += 1;
	    (*count_word_context)[bag_context][word] += 1;
	} else if (context_definition_ == "list") {  // List-of-words (LOW)
	    Context list_context =
		AddContextIfUnknown(position_markers.at(context_index) +
				    context_string);
	    (*count_context)[list_context] += 1;
	    (*count_word_context)[list_context][word] += 1;
	} else if (context_definition_ == "baglist") {  // BOW+LOW
	    Context bag_context = AddContextIfUnknown(context_string);
	    Context list_context =
		AddContextIfUnknown(position_markers.at(context_index) +
				    context_string);
	    (*count_context)[bag_context] += 1;
	    (*count_context)[list_context] += 1;
	    (*count_word_context)[bag_context][word] += 1;
	    (*count_word_context)[list_context][word] += 1;
	} else {
	    ASSERT(false, "Unknown context definition: " <<
		   context_definition_);
	}
    }
}

Context WordRep::AddContextIfUnknown(const string &context_string) {
    ASSERT(!context_string.empty(), "Adding an empty string for context!");
    if (context_str2num_.find(context_string) == context_str2num_.end()) {
	Context context = context_str2num_.size();
	context_str2num_[context_string] = context;
	context_num2str_[context] = context_string;
    }
    return context_str2num_[context_string];
}

void WordRep::InduceWordVectors() {
    FileManipulator file_manipulator;  // Do not repeat the work.
    if (!file_manipulator.Exists(WordVectorsPath())) {
	CalculateSVD();
	if (weighting_method_ != "") { CalculateWeightedLeastSquares(); }
	ASSERT(word_matrix_.rows() == sorted_wordcount_.size(), "Word matrix "
	       "dimension and vocabulary size mismatch: " << word_matrix_.rows()
	       << " vs " << sorted_wordcount_.size());

	ofstream wordvectors_file(WordVectorsPath(), ios::out);
	for (size_t i = 0; i < sorted_wordcount_.size(); ++i) {
	    string word_string = sorted_wordcount_[i].first;
	    size_t word_count = sorted_wordcount_[i].second;
	    Word word = word_str2num_[word_string];
	    word_matrix_.row(word).normalize();  // Normalize word vectors.
	    wordvectors_[word_string] = word_matrix_.row(word);
	    wordvectors_file << word_count << " " << word_string;
	    for (size_t j = 0; j < wordvectors_[word_string].size(); ++ j) {
		wordvectors_file << " " << wordvectors_[word_string](j);
	    }
	    wordvectors_file << endl;
	}
    } else {  // Load word vectors.
	StringManipulator string_manipulator;
	string line;
	vector<string> tokens;
	wordvectors_.clear();
	ifstream wordvectors_file(WordVectorsPath(), ios::in);
	while (wordvectors_file.good()) {
	    getline(wordvectors_file, line);
	    if (line == "") { continue; }

	    // line = [count] [word_string] [value_{1}] ... [value_{dim_}]
	    string_manipulator.Split(line, " ", &tokens);
	    Eigen::VectorXd vector(tokens.size() - 2);
	    for (size_t i = 0; i < tokens.size() - 2; ++i) {
		vector(i) = stod(tokens[i + 2]);
	    }
	    wordvectors_[tokens[1]] = vector;
	}
    }
}

void WordRep::LoadWordDictionary() {
    FileManipulator file_manipulator;
    ASSERT(file_manipulator.Exists(WordStr2NumPath()), "File not found, "
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
	string_manipulator.Split(line, " ", &tokens);
	word_num2str_[stol(tokens[1])] = tokens[0];
	word_str2num_[tokens[0]] = stol(tokens[1]);
    }
}

void WordRep::LoadSortedWordCounts() {
    FileManipulator file_manipulator;
    ASSERT(file_manipulator.Exists(SortedWordTypesPath()), "File not found, "
	   "read from the corpus: " << SortedWordTypesPath());
    ASSERT(word_str2num_.size() > 0, "Word dictionary not loaded.");

    ifstream sorted_word_types_file(SortedWordTypesPath(), ios::in);
    string line;
    vector<string> tokens;
    StringManipulator string_manipulator;
    unordered_map<string, size_t> wordcount;
    while (sorted_word_types_file.good()) {
	getline(sorted_word_types_file, line);
	if (line == "") { continue; }
	string_manipulator.Split(line, " ", &tokens);
	string word_string = tokens[0];
	size_t word_count = stol(tokens[1]);
	if (word_str2num_.find(word_string) != word_str2num_.end()) {
	    wordcount[word_string] = word_count;
	} else {
	    wordcount[kRareString_] += word_count;
	}
    }
    sorted_wordcount_.clear();
    for (const auto &word_count_pair : wordcount) {
	sorted_wordcount_.push_back(word_count_pair);
    }
    sort(sorted_wordcount_.begin(), sorted_wordcount_.end(),
	 sort_pairs_second<string, size_t, greater<size_t> >());
}

void WordRep::CalculateSVD() {
    FileManipulator file_manipulator;
    ASSERT(file_manipulator.Exists(CountWordContextPath()), "File not found, "
	   "read from the corpus: " << CountWordContextPath());
    ASSERT(file_manipulator.Exists(CountWordPath()), "File not found, "
	   "read from the corpus: " << CountWordPath());
    ASSERT(file_manipulator.Exists(CountContextPath()), "File not found, "
	   "read from the corpus: " << CountContextPath());
    StringManipulator string_manipulator;
    string line;
    vector<string> tokens;

    // Get information about the count matrix.
    ifstream count_word_context_file(CountWordContextPath(), ios::in);
    getline(count_word_context_file, line);
    string_manipulator.Split(line, " ", &tokens);
    size_t dim1 = stol(tokens[0]);
    size_t dim2 = stol(tokens[1]);
    size_t num_nonzeros = stol(tokens[2]);

    // Get the number of samples (= number of words).
    size_t num_samples = 0;
    ifstream count_word_file(CountWordPath(), ios::in);
    while (count_word_file.good()) {
	getline(count_word_file, line);
	if (line == "") { continue; }
	string_manipulator.Split(line, " ", &tokens);
	num_samples += stol(tokens[0]);
    }

    log_ << endl << "[Decomposing a matrix of scaled counts]" << endl;
    log_ << "   Matrix: " << dim1 << " x " << dim2 << " (" << num_nonzeros
	 << " nonzeros)" << endl;
    log_ << "   Rank of SVD: " << dim_ << endl;
    log_ << "   Transformation: " << transformation_method_ << endl;
    log_ << "   Scaling: " << scaling_method_ << endl;

    time_t begin_time_decomposition = time(NULL);

    // Load a sparse matrix of joint values directly into an SVD solver.
    SparseSVDSolver svd_solver(CountWordContextPath());
    SMat matrix = svd_solver.sparse_matrix();

    // Load individual scaling values.
    unordered_map<size_t, double> values1;
    unordered_map<size_t, double> values2;
    file_manipulator.Read(CountWordPath(), &values1);
    file_manipulator.Read(CountContextPath(), &values2);
    ASSERT(dim1 == values1.size() && dim2 == values2.size(),
	   "Dimensions don't match, need: " << dim1 << " = " << values1.size()
	   << " && " << dim2 << " = " << values2.size());

    // Scale the joint values by individual values.
    for (size_t col = 0; col < dim2; ++col) {
	size_t current_column_nonzero_index = matrix->pointr[col];
	size_t next_column_start_nonzero_index = matrix->pointr[col + 1];
	while (current_column_nonzero_index < next_column_start_nonzero_index) {
	    size_t row = matrix->rowind[current_column_nonzero_index];
	    matrix->value[current_column_nonzero_index] =
		ScaleJointValue(matrix->value[current_column_nonzero_index],
				values1[row], values2[col], num_samples);
	    ++current_column_nonzero_index;
	}
    }

    // Perform an SVD on the loaded scaled values.
    svd_solver.SolveSparseSVD(dim_);
    size_t actual_rank = svd_solver.rank();

    // Save singular values.
    singular_values_.resize(dim_);
    for (size_t i = 0; i < dim_; ++i) {
	singular_values_(i) = *(svd_solver.singular_values() + i);
    }

    // Save a matrix of left singular vectors as columns.
    word_matrix_.resize(dim1, dim_);
    for (size_t row = 0; row < dim1; ++row) {
	for (size_t col = 0; col < dim_; ++col) {
	    word_matrix_(row, col) =
		svd_solver.left_singular_vectors()->value[col][row];
	}
    }

    // Save a matrix of right singular vectors, scaled by singular values, as
    // columns.
    context_matrix_.resize(dim2, dim_);
    for (size_t row = 0; row < dim2; ++row) {
	for (size_t col = 0; col < dim_; ++col) {
	    context_matrix_(row, col) =
		svd_solver.right_singular_vectors()->value[col][row] *
		singular_values_(col);
	}
    }

    // Free memory.
    svd_solver.FreeSparseMatrix();
    svd_solver.FreeSVDResult();

    double time_decomposition = difftime(time(NULL), begin_time_decomposition);
    if (actual_rank < dim_) {
	log_ << "   ***WARNING*** The matrix has defficient rank "
	     << actual_rank << " < " << dim_ << "!" << endl;
    }

    log_ << "   Condition number: "
	 << singular_values_[0] / singular_values_[dim_ - 1] << endl;
    log_ << "   Time taken: "
	 << string_manipulator.TimeString(time_decomposition) << endl;

    // Write singular values.
    file_manipulator.Write(singular_values_, SingularValuesPath());
}

double WordRep::ScaleJointValue(double joint_value, double value1,
				double value2, size_t num_samples) {
    value2 = pow(value2, 0.75);  // Context smoothing.

    // Data transformation.
    if (transformation_method_ == "raw") {  // No transformation.
    } else if (transformation_method_ == "sqrt") {  // Take square-root.
	joint_value = sqrt(joint_value);
	value1 = sqrt(value1);
	value2 = sqrt(value2);
    } else if (transformation_method_ == "two-thirds") {  // Power of 2/3.
	double power = 2.0 / 3.0;
	joint_value = pow(joint_value, power);
	value1 = pow(value1, power);
	value2 = pow(value2, power);
    } else if (transformation_method_ == "log") {  // Take log.
	joint_value = log(1.0 + joint_value);
	value1 = log(1.0 + value1);
	value2 = log(1.0 + value2);
    } else {
	ASSERT(false, "Unknown data transformation method: "
	       << transformation_method_);
    }

    // Scale the joint value by individual values (or not).
    double scaled_joint_value = joint_value;
    if (scaling_method_ == "raw") {  // No scaling.
    } else if (scaling_method_ == "cca") {
	// Canonical correlation analysis scaling.
	scaled_joint_value /= sqrt(value1);
	scaled_joint_value /= sqrt(value2);
    } else if (scaling_method_ == "reg") {
	// Ridge regression scaling.
	scaled_joint_value /= value1;
    } else if (scaling_method_ == "ppmi") {
	// Positive pointwise mutual information scaling.
	scaled_joint_value = log(scaled_joint_value);
	scaled_joint_value += log(num_samples);
	scaled_joint_value -= log(value1);
	scaled_joint_value -= log(value2);
	scaled_joint_value = max(scaled_joint_value, 0.0);
    } else {
	ASSERT(false, "Unknown scaling method: " << scaling_method_);
    }
    return scaled_joint_value;
}

void WordRep::CalculateWeightedLeastSquares() {
    // Load scaling values.
    FileManipulator file_manipulator;
    unordered_map<size_t, double> values1;
    unordered_map<size_t, double> values2;
    file_manipulator.Read(CountWordPath(), &values1);
    file_manipulator.Read(CountContextPath(), &values2);

    // Get the number of samples (= number of words).
    StringManipulator string_manipulator;
    string line;
    vector<string> tokens;
    size_t num_samples = 0;
    ifstream count_word_file(CountWordPath(), ios::in);
    while (count_word_file.good()) {
	getline(count_word_file, line);
	if (line == "") { continue; }
	string_manipulator.Split(line, " ", &tokens);
	num_samples += stol(tokens[0]);
    }

    // Prepare the column-major format.
    unordered_map<size_t, vector<tuple<size_t, double, double> > > col2row;
    ifstream count_word_context_file(CountWordContextPath(), ios::in);
    getline(count_word_context_file, line);
    string_manipulator.Split(line, " ", &tokens);
    size_t num_columns = stol(tokens[1]);

    double max_weight = -numeric_limits<double>::infinity();
    double min_weight = numeric_limits<double>::infinity();
    for (size_t col = 0; col < num_columns; ++col) {
	getline(count_word_context_file, line);  // Number of nonzero rows.
	string_manipulator.Split(line, " ", &tokens);
	size_t num_nonzero_rows = stol(tokens[0]);
	for (size_t nonzero_row = 0; nonzero_row < num_nonzero_rows;
	     ++nonzero_row) {
	    getline(count_word_context_file, line);  // <row> <value>
	    string_manipulator.Split(line, " ", &tokens);
	    size_t row = stol(tokens[0]);
	    size_t cocount = stod(tokens[1]);
	    double value = ScaleJointValue(cocount, values1[row],
					   values2[col], num_samples);
	    double weight = sqrt(values1[row]) * sqrt(values2[col]) /
		sqrt(cocount);
	    if (weight > max_weight) { max_weight = weight; }
	    if (weight < min_weight) { min_weight = weight; }
	    col2row[col].emplace_back(row, value, weight);
	}
    }

    //cout << max_weight << endl;
    //cout << min_weight << endl;
    // Normalize weights.
    double new_min_weight = 0.01 * max_weight;
    double new_max_weight = 0.8 * max_weight;
    vector<pair<pair<string, string>, double> > temp;
    for (const auto &col_pair: col2row) {
	size_t col = col_pair.first;
	for (size_t i = 0; i < col_pair.second.size(); ++i) {
	    double weight = get<2>(col2row[col][i]);
	    if (weight < new_min_weight) {
		get<2>(col2row[col][i]) = new_min_weight;
	    } else if (weight > new_max_weight) {
		get<2>(col2row[col][i]) = new_max_weight;
	    }
	    get<2>(col2row[col][i]) /= new_max_weight;
	    string wordstr = word_num2str_[get<0>(col2row[col][i])];
	    string contextstr = context_num2str_[col];
	    temp.push_back(make_pair(make_pair(wordstr, contextstr),
				     get<2>(col2row[col][i])));
	}
    }
    /*
    sort(temp.begin(), temp.end(),
	 sort_pairs_second<pair<string, string>, double, greater<double> >());
    for (size_t i = 0; i < temp.size(); ++i) {
	cout << temp[i].first.first << " " << temp[i].first.second << " "
	     << temp[i].second << endl;
    }
    */

    // Prepare the row-major format (flip the column-major format).
    unordered_map<size_t, vector<tuple<size_t, double, double> > > row2col;
    for (const auto &col_pair: col2row) {
	size_t col = col_pair.first;
	for (const auto &row_tuple: col_pair.second) {
	    size_t row = get<0>(row_tuple);
	    double value = get<1>(row_tuple);
	    double weight = get<2>(row_tuple);
	    row2col[row].emplace_back(col, value, weight);
	}
    }

    WSQLossOptimizer wsqloss_optimizer;
    wsqloss_optimizer.Optimize(col2row, row2col, &word_matrix_,
			       &context_matrix_);
}

void WordRep::TestQualityOfWordVectors() {
    string wordsim353_path = "third_party/public_datasets/wordsim353.dev";
    string men_path = "third_party/public_datasets/men.dev";
    string syn_path = "third_party/public_datasets/syntactic_analogies.dev";
    string mixed_path = "third_party/public_datasets/mixed_analogies.dev";
    FileManipulator file_manipulator;
    if (!file_manipulator.Exists(wordsim353_path) ||
	!file_manipulator.Exists(men_path) ||
	!file_manipulator.Exists(syn_path) ||
	!file_manipulator.Exists(mixed_path)) {
	// Skip evaluation (e.g., in unit tests) if files are not found.
	return;
    }
    log_ << endl << "[Dev performance]" << endl;

    // Use 3 decimal places for word similartiy.
    log_ << fixed << setprecision(3);

    // Word similarity with wordsim353.dev.
    size_t num_instances_wordsim353;
    size_t num_handled_wordsim353;
    double corr_wordsim353;
    EvaluateWordSimilarity(wordsim353_path, &num_instances_wordsim353,
			   &num_handled_wordsim353, &corr_wordsim353);
    log_ << "   WS353: \t" << corr_wordsim353 << " ("
	 << num_handled_wordsim353 << "/" << num_instances_wordsim353
	 << " evaluated)" << endl;

    // Word similarity with men.dev.
    size_t num_instances_men;
    size_t num_handled_men;
    double corr_men;
    EvaluateWordSimilarity(men_path, &num_instances_men,
			   &num_handled_men, &corr_men);
    log_ << "   MEN: \t" << corr_men << " (" << num_handled_men << "/"
	 << num_instances_men << " evaluated)" << endl;
    log_ << fixed << setprecision(2);

    // Word analogy with syntactic_analogies.dev.
    size_t num_instances_syn;
    size_t num_handled_syn;
    double acc_syn;
    EvaluateWordAnalogy(syn_path, &num_instances_syn, &num_handled_syn,
			&acc_syn);
    log_ << "   SYN: \t" << acc_syn << " (" << num_handled_syn
	 << "/" << num_instances_syn << " evaluated)" << endl;

    // Word analogy with mixed_analogies.dev.
    size_t num_instances_mixed;
    size_t num_handled_mixed;
    double acc_mixed;
    EvaluateWordAnalogy(mixed_path, &num_instances_mixed, &num_handled_mixed,
			&acc_mixed);
    log_ << "   MIXED: \t" << acc_mixed << " (" << num_handled_mixed << "/"
	 << num_instances_mixed << " evaluated)" << endl;
}

void WordRep::EvaluateWordSimilarity(const string &file_path,
				     size_t *num_instances, size_t *num_handled,
				     double *correlation) {
    ifstream similarity_file(file_path, ios::in);
    ASSERT(similarity_file.is_open(), "Cannot open file: " << file_path);
    StringManipulator string_manipulator;
    string line;
    vector<string> tokens;
    vector<double> human_scores;
    vector<double> cosine_scores;
    *num_instances = 0;
    *num_handled = 0;
    while (similarity_file.good()) {
	getline(similarity_file, line);
	if (line == "") { continue; }
	++(*num_instances);
	string_manipulator.Split(line, " ", &tokens);
	ASSERT(tokens.size() == 3, "Wrong format for word similarity!");
	string word1 = tokens[0];
	string word2 = tokens[1];
	string word1_lowercase = string_manipulator.Lowercase(word1);
	string word2_lowercase = string_manipulator.Lowercase(word2);
	double human_score = stod(tokens[2]);

	// Get a vector for each word type. First, try to get a vector for the
	// original string. If not found, try lowercasing.
	Eigen::VectorXd vector_word1, vector_word2;
	if (wordvectors_.find(word1) != wordvectors_.end()) {
	    vector_word1 = wordvectors_[word1];
	} else if (wordvectors_.find(word1_lowercase) != wordvectors_.end()) {
	    vector_word1 = wordvectors_[word1_lowercase];
	}
	if (wordvectors_.find(word2) != wordvectors_.end()) {
	    vector_word2 = wordvectors_[word2];
	} else if (wordvectors_.find(word2_lowercase) != wordvectors_.end()) {
	    vector_word2 = wordvectors_[word2_lowercase];
	}

	// If we have vectors for both word types, compute similarity.
	if (vector_word1.size() > 0 && vector_word2.size() > 0) {
	    // Assumes that word vectors already have length 1.
	    double cosine_score = vector_word1.dot(vector_word2);
	    human_scores.push_back(human_score);
	    cosine_scores.push_back(cosine_score);
	    ++(*num_handled);
	}
    }
    Stat stat;
    *correlation = stat.ComputeSpearman(human_scores, cosine_scores);
}

void WordRep::EvaluateWordAnalogy(const string &file_path,
				  size_t *num_instances, size_t *num_handled,
				  double *accuracy) {
    // Read analogy questions and their vocabulary.
    ifstream analogy_file(file_path, ios::in);
    ASSERT(analogy_file.is_open(), "Cannot open file: " << file_path);
    StringManipulator string_manipulator;
    string line;
    vector<string> tokens;
    vector<tuple<string, string, string, string> > analogies;
    unordered_map<string, Eigen::VectorXd> wordvectors_subset;
    while (analogy_file.good()) {
	getline(analogy_file, line);
	if (line == "") { continue; }
	string_manipulator.Split(line, " ", &tokens);
	ASSERT(tokens.size() == 5, "Wrong format for word analogy!");
	// Ignore the analogy category: only compute the overall accuracy.
	string w1 = tokens[1];
	string w1_lowercase = string_manipulator.Lowercase(w1);
	string w2 = tokens[2];
	string w2_lowercase = string_manipulator.Lowercase(w2);
	string v1 = tokens[3];
	string v1_lowercase = string_manipulator.Lowercase(v1);
	string v2 = tokens[4];
	string v2_lowercase = string_manipulator.Lowercase(v2);
	analogies.push_back(make_tuple(w1, w2, v1, v2));

	// Get a vector for each word type. First, try to get a vector for the
	// original string. If not found, try lowercasing.
	if (wordvectors_.find(w1) != wordvectors_.end()) {
	    wordvectors_subset[w1] = wordvectors_[w1];
	} else if (wordvectors_.find(w1_lowercase) != wordvectors_.end()) {
	    wordvectors_subset[w1] = wordvectors_[w1_lowercase];
	}
	if (wordvectors_.find(w2) != wordvectors_.end()) {
	    wordvectors_subset[w2] = wordvectors_[w2];
	} else if (wordvectors_.find(w2_lowercase) != wordvectors_.end()) {
	    wordvectors_subset[w2] = wordvectors_[w2_lowercase];
	}
	if (wordvectors_.find(v1) != wordvectors_.end()) {
	    wordvectors_subset[v1] = wordvectors_[v1];
	} else if (wordvectors_.find(v1_lowercase) != wordvectors_.end()) {
	    wordvectors_subset[v1] = wordvectors_[v1_lowercase];
	}
	if (wordvectors_.find(v2) != wordvectors_.end()) {
	    wordvectors_subset[v2] = wordvectors_[v2];
	} else if (wordvectors_.find(v2_lowercase) != wordvectors_.end()) {
	    wordvectors_subset[v2] = wordvectors_[v2_lowercase];
	}
    }

    // For each analogy question "w1:w2 as in v1:v2" such that we have vector
    // representations for word types w1, w2, v1, v2, predict v2.
    *num_instances = 0;
    *num_handled = 0;
    size_t num_correct = 0;
    for (const auto &word_quadruple : analogies) {
	++(*num_instances);
	string w1 = get<0>(word_quadruple);
	string w2 = get<1>(word_quadruple);
	string v1 = get<2>(word_quadruple);
	string v2 = get<3>(word_quadruple);
	if (wordvectors_subset.find(w1) != wordvectors_subset.end() &&
	    wordvectors_subset.find(w2) != wordvectors_subset.end() &&
	    wordvectors_subset.find(v1) != wordvectors_subset.end() &&
	    wordvectors_subset.find(v2) != wordvectors_subset.end()) {
	    ++(*num_handled);
	    string predicted_v2 = AnswerAnalogyQuestion(w1, w2, v1,
							wordvectors_subset);
	    if (predicted_v2 == v2) { ++num_correct; }
	}
    }
    *accuracy = ((double) num_correct) / (*num_handled) * 100.0;
}

string WordRep::AnswerAnalogyQuestion(
    string w1, string w2, string v1,
    const unordered_map<string, Eigen::VectorXd> &wordvectors_subset) {
    ASSERT(wordvectors_subset.find(w1) != wordvectors_subset.end(),
	   "No vector for " << w1);
    ASSERT(wordvectors_subset.find(w2) != wordvectors_subset.end(),
	   "No vector for " << w2);
    ASSERT(wordvectors_subset.find(v1) != wordvectors_subset.end(),
	   "No vector for " << v1);
    // Assumes vectors are already normalized to have length 1.
    Eigen::VectorXd w1_embedding = wordvectors_subset.at(w1);
    Eigen::VectorXd w2_embedding = wordvectors_subset.at(w2);
    Eigen::VectorXd v1_embedding = wordvectors_subset.at(v1);
    string predicted_v2 = "";
    double max_score = -numeric_limits<double>::max();
    for (const auto &word_vector_pair : wordvectors_subset) {
	string word = word_vector_pair.first;
	if (word == w1 || word == w2 || word == v1) { continue; }
	Eigen::VectorXd word_embedding = word_vector_pair.second;
	double shifted_cos_w1 =
	    (word_embedding.dot(w1_embedding) + 1.0) / 2.0;
	double shifted_cos_w2 =
	    (word_embedding.dot(w2_embedding) + 1.0) / 2.0;
	double shifted_cos_v1 =
	    (word_embedding.dot(v1_embedding) + 1.0) / 2.0;
	double score =
	    shifted_cos_w2 * shifted_cos_v1 / (shifted_cos_w1 + 0.001);
	if (score > max_score) {
	    max_score = score;
	    predicted_v2 = word;
	}
    }
    ASSERT(!predicted_v2.empty(), "No answer for \"" << w1 << ":" << w2
	   << " as in " << v1 << ":" << "?\"");
    return predicted_v2;
}

void WordRep::PerformAgglomerativeClustering(size_t num_clusters) {
    FileManipulator file_manipulator;  // Do not repeat the work.
    if (file_manipulator.Exists(AgglomerativePath())) { return; }

    // Prepare a list of word vectors sorted in decreasing frequency.
    ASSERT(wordvectors_.size() > 0, "No word vectors to cluster!");
    vector<Eigen::VectorXd> sorted_vectors(sorted_wordcount_.size());
    for (size_t i = 0; i < sorted_wordcount_.size(); ++i) {
	string word_string = sorted_wordcount_[i].first;
	sorted_vectors[i] = wordvectors_[word_string];
    }

    // Do agglomerative clustering over the sorted word vectors.
    time_t begin_time_greedo = time(NULL);
    log_ << endl << "[Agglomerative clustering]" << endl;
    log_ << "   Number of clusters: " << num_clusters << endl;
    Greedo greedo;
    greedo.Cluster(sorted_vectors, num_clusters);
    double time_greedo = difftime(time(NULL), begin_time_greedo);
    StringManipulator string_manipulator;
    log_ << "   Average number of tightenings: "
	 << greedo.average_num_extra_tightening() << " (versus exhaustive "
	 << num_clusters << ")" << endl;
    log_ << "   Time taken: " << string_manipulator.TimeString(time_greedo)
	 << endl;

    // Lexicographically sort bit strings for enhanced readability.
    vector<string> bitstring_types;
    for (const auto &bitstring_pair : *greedo.bit2cluster()) {
	bitstring_types.push_back(bitstring_pair.first);
    }
    sort(bitstring_types.begin(), bitstring_types.end());

    // Write the bit strings and their associated word types.
    ofstream greedo_file(AgglomerativePath(), ios::out);
    unordered_map<string, vector<size_t> > *bit2cluster = greedo.bit2cluster();
    for (const auto &bitstring : bitstring_types) {
	vector<pair<string, size_t> > sorting_vector;  // Sort each cluster.
	for (size_t cluster : bit2cluster->at(bitstring)) {
	    string word_string = sorted_wordcount_[cluster].first;
	    size_t count = sorted_wordcount_[cluster].second;
	    sorting_vector.push_back(make_pair(word_string, count));
	}
	sort(sorting_vector.begin(), sorting_vector.end(),
	     sort_pairs_second<string, size_t, greater<size_t> >());

	for (const auto &word_pair : sorting_vector) {
	    greedo_file << bitstring << " " << word_pair.first << " "
			<< word_pair.second << endl;
	}
    }
}

string WordRep::Signature(size_t version) {
    ASSERT(version <= 2, "Unrecognized signature version: " << version);

    string signature = "rare" + to_string(rare_cutoff_);
    if (version >= 1) {
	if (sentence_per_line_) {
	    signature += "_spl";
	}
	signature += "_window" + to_string(window_size_);
	signature += "_" + context_definition_;
    }
    if (version >= 2) {
	signature += "_dim" + to_string(dim_);
	signature += "_" + transformation_method_;
	signature += "_" + scaling_method_;
    }
    return signature;
}
